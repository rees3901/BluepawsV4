#include "home_hub_web.h"

#include "bluepaws/hub_settings.h"
#include "home_hub_defaults.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

namespace bluepaws::web {
namespace {

constexpr char kTag[] = "home_hub_web";
constexpr char kWebRoot[] = "/web";
constexpr char kHubId[] = "0010";

struct MapLayer {
    const char *id;
    const char *name;
    const char *root;
    uint8_t minimum_zoom;
    uint8_t maximum_zoom;
};

constexpr MapLayer kMapLayers[] = {
    {"osm", "OpenStreetMap", "/sdcard/bluepaws/maps/layers/osm-road-100km/tiles", 5, 17},
    {"os", "Ordnance Survey", "/sdcard/bluepaws/maps/layers/ordnance-survey-100km/tiles", 5, 17},
    {"satellite", "Satellite", "/sdcard/bluepaws/maps/layers/satellite-v2/tiles", 14, 17},
    {"aerial", "Aerial", "/sdcard/bluepaws/maps/layers/aerial-consistent/tiles", 12, 17},
};

struct WebSnapshot {
    std::array<CatRecord, kMaximumCats> cats{};
    std::size_t count = 0;
    cloud::Status cloud{};
};

SemaphoreHandle_t g_lock = nullptr;
httpd_handle_t g_server = nullptr;
WebSnapshot g_snapshot{};
std::atomic_bool g_starting{false};
std::atomic_int g_requested_mode{-1};

const char *mode_name(hub::CommunicationsMode mode)
{
    switch (mode) {
    case hub::CommunicationsMode::Portable: return "portable";
    case hub::CommunicationsMode::OffGrid: return "off_grid";
    default: return "home";
    }
}

const char *status_name(uint8_t status)
{
    switch (status) {
    case 0: return "Home";
    case 2: return "Lost";
    case 3: return "Error";
    default: return "Out";
    }
}

const char *profile_name(uint8_t profile)
{
    switch (profile) {
    case 0: return "PowerSave";
    case 2: return "Active";
    case 3: return "Emergency";
    case 4: return "Debug";
    default: return "Normal";
    }
}

WebSnapshot snapshot()
{
    WebSnapshot copy{};
    if (g_lock != nullptr && xSemaphoreTake(g_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        copy = g_snapshot;
        xSemaphoreGive(g_lock);
    }
    return copy;
}

esp_err_t send_json(httpd_req_t *request, cJSON *json, const char *status = nullptr)
{
    if (status != nullptr) httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    char *text = cJSON_PrintUnformatted(json);
    if (text == nullptr) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                                    "JSON allocation failed");
    const esp_err_t result = httpd_resp_sendstr(request, text);
    cJSON_free(text);
    return result;
}

void add_device_json(cJSON *array, const CatRecord &cat)
{
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "id", cat.device_id);
    cJSON_AddStringToObject(item, "name", cat.name[0] == '\0' ? "Collar" : cat.name);
    // Empty means the offline UI assigns a stable, device-specific animal avatar.
    // "BP" used to make every unconfigured collar look identical.
    cJSON_AddStringToObject(item, "emoji", cat.appearance.emoji);
    cJSON_AddStringToObject(item, "colour", cat.appearance.marker_colour);
    cJSON_AddNumberToObject(item, "seq", cat.latest.sequence);
    cJSON_AddNumberToObject(item, "time", cat.latest.observed_at);
    cJSON_AddStringToObject(item, "status", status_name(cat.latest.status_code));
    cJSON_AddStringToObject(item, "profile", profile_name(cat.latest.power_profile_code));
    cJSON_AddBoolToObject(item, "errorPresent",
                          cat.latest.status_code == 3 || (cat.latest.flags & 0x80U) != 0);
    cJSON_AddNumberToObject(item, "flags", cat.latest.flags);
    cJSON_AddNumberToObject(item, "txReasonCode", cat.latest.tx_reason);
    cJSON_AddNumberToObject(item, "lat",
                            cat.has_position ? cat.last_valid_latitude_e7 / 1.0e7 : 0.0);
    cJSON_AddNumberToObject(item, "lon",
                            cat.has_position ? cat.last_valid_longitude_e7 / 1.0e7 : 0.0);
    cJSON_AddBoolToObject(item, "hasGps", cat.has_position);
    cJSON_AddNumberToObject(item, "batt", cat.latest.battery_mv);
    cJSON_AddNumberToObject(item, "acc", 0);
    cJSON_AddNumberToObject(item, "fixAge", 0);
    cJSON_AddNumberToObject(item, "rssi", cat.latest.rssi);
    cJSON_AddNumberToObject(item, "snr", cat.latest.snr);
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const uint32_t age = now_ms >= cat.latest.received_at_ms
        ? (now_ms - cat.latest.received_at_ms) / 1000U : 0;
    cJSON_AddNumberToObject(item, "age", age);
    cJSON_AddNumberToObject(item, "rxWindowMs", age >= 10U ? 0U : (10U - age) * 1000U);
    cJSON_AddBoolToObject(item, "stale", age >= 600U);
    cJSON_AddNumberToObject(item, "localId", cat.latest.revision);
    cJSON_AddNumberToObject(item, "gatewayRxTime", cat.latest.observed_at);
    cJSON_AddStringToObject(item, "verification",
                            cat.latest.source == TelemetrySource::Cloud ? "validated" : "pending");
    cJSON_AddItemToArray(array, item);
}

esp_err_t devices_handler(httpd_req_t *request)
{
    const WebSnapshot state = snapshot();
    cJSON *array = cJSON_CreateArray();
    if (array == nullptr) return ESP_ERR_NO_MEM;
    for (std::size_t i = 0; i < state.count; ++i) add_device_json(array, state.cats[i]);
    const esp_err_t result = send_json(request, array);
    cJSON_Delete(array);
    return result;
}

esp_err_t welcome_api_handler(httpd_req_t *request)
{
    const WebSnapshot state = snapshot();
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    uint32_t youngest = UINT32_MAX;
    uint32_t recent = 0;
    for (std::size_t i = 0; i < state.count; ++i) {
        const uint32_t received = state.cats[i].latest.received_at_ms;
        const uint32_t age = now_ms >= received ? (now_ms - received) / 1000U : 0;
        if (age <= 600U) ++recent;
        if (age < youngest) youngest = age;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "hub_id", kHubId);
    cJSON_AddNumberToObject(json, "recent_collars", recent);
    cJSON_AddNumberToObject(json, "known_collars", state.count);
    if (youngest == UINT32_MAX) cJSON_AddNullToObject(json, "last_report_age_s");
    else cJSON_AddNumberToObject(json, "last_report_age_s", youngest);
    cJSON_AddBoolToObject(json, "time_synced", state.cloud.time_synchronized);
    const esp_err_t result = send_json(request, json);
    cJSON_Delete(json);
    return result;
}

esp_err_t status_handler(httpd_req_t *request)
{
    const WebSnapshot state = snapshot();
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "uptime", esp_timer_get_time() / 1000000);
    // Tells the shared web UI that this endpoint is the physical hub itself.
    // Connection confidence must therefore reflect browser-to-hub reachability,
    // not Internet or cloud state.
    cJSON_AddBoolToObject(json, "localLink", true);
    cJSON_AddNumberToObject(json, "devices", state.count);
    cJSON_AddNumberToObject(json, "freeHeap", esp_get_free_heap_size());
    cJSON_AddStringToObject(json, "mode", mode_name(state.cloud.effective_mode));
    cJSON_AddStringToObject(json, "hubMode", mode_name(state.cloud.effective_mode));
    cJSON_AddBoolToObject(json, "wifi_connected",
                          state.cloud.state == cloud::ConnectionState::Online);
    cJSON_AddBoolToObject(json, "internet_reachable",
                          state.cloud.state == cloud::ConnectionState::Online);
    cJSON_AddBoolToObject(json, "cloud_reachable",
                          state.cloud.state == cloud::ConnectionState::Online);
    cJSON_AddBoolToObject(json, "apEnabled",
                          state.cloud.effective_mode == hub::CommunicationsMode::OffGrid);
    cJSON_AddStringToObject(json, "apIP", "192.168.4.1");
    cJSON_AddBoolToObject(json, "time_synced", state.cloud.time_synchronized);
    cJSON_AddNumberToObject(json, "hub_time_unix", std::time(nullptr));
    cJSON_AddBoolToObject(json, "lora_rx_active", false);
    cJSON_AddNumberToObject(json, "replay_backlog", 0);
    cJSON_AddNumberToObject(json, "snapshot_http", state.cloud.last_http_status);
    cJSON_AddNumberToObject(json, "rxCount", 0);
    cJSON_AddNumberToObject(json, "txCount", 0);
    cJSON_AddNumberToObject(json, "crcFails", 0);
    cJSON_AddNumberToObject(json, "logEntries", 0);
    cJSON_AddBoolToObject(json, "staConnected",
                          state.cloud.state == cloud::ConnectionState::Online);
    cJSON_AddStringToObject(json, "staIP", "");
    cJSON_AddNumberToObject(json, "recovery_remaining_ms", 0);
    cJSON_AddNumberToObject(json, "ap_clients", 0);
    cJSON_AddNumberToObject(json, "ap_channel", 0);
    cJSON_AddNumberToObject(json, "ap_start_failures", 0);
    cJSON_AddNumberToObject(json, "network_stack_free", 0);
    cJSON_AddNumberToObject(json, "web_stack_free", 0);
    cJSON_AddBoolToObject(json, "known_wifi_available", false);
    cJSON_AddBoolToObject(json, "provisioning_mode", false);
    cJSON_AddStringToObject(json, "network_phase",
                            state.cloud.state == cloud::ConnectionState::Online
                                ? "connected" : "off_grid");
    const esp_err_t result = send_json(request, json);
    cJSON_Delete(json);
    return result;
}

esp_err_t empty_array_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, "[]");
}

esp_err_t hub_presence_handler(httpd_req_t *request)
{
    const WebSnapshot state = snapshot();
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "gateway_guid16", kHubId);
    cJSON_AddStringToObject(json, "display_name", "Home Hub");
    cJSON_AddStringToObject(json, "mode", mode_name(state.cloud.effective_mode));
    cJSON_AddStringToObject(json, "reporting_profile", "normal");
    cJSON_AddNumberToObject(json, "latitude", defaults::kStarterLocation.latitude);
    cJSON_AddNumberToObject(json, "longitude", defaults::kStarterLocation.longitude);
    cJSON_AddStringToObject(json, "position_source", "starter");
    cJSON_AddNullToObject(json, "fix_age_s");
    cJSON_AddNullToObject(json, "wifi_rssi_dbm");
    cJSON_AddBoolToObject(json, "ble_advertising", false);
    cJSON_AddBoolToObject(json, "ble_enabled", false);
    cJSON_AddBoolToObject(json, "ble_settled", true);
    cJSON_AddNumberToObject(json, "uptime_s", esp_timer_get_time() / 1000000);
    cJSON_AddStringToObject(json, "home_emoji", "Home");
    cJSON_AddStringToObject(json, "portable_emoji", "Hub");
    cJSON_AddStringToObject(json, "marker_colour", "#38bdf8");
    cJSON_AddNumberToObject(json, "control_poll_s", 0);
    const esp_err_t result = send_json(request, json);
    cJSON_Delete(json);
    return result;
}

bool directory_exists(const char *path)
{
    struct stat details{};
    return stat(path, &details) == 0 && S_ISDIR(details.st_mode);
}

esp_err_t map_layers_handler(httpd_req_t *request)
{
    cJSON *response = cJSON_CreateObject();
    if (response == nullptr) return ESP_ERR_NO_MEM;
    cJSON *layers = cJSON_AddArrayToObject(response, "layers");
    if (layers == nullptr) {
        cJSON_Delete(response);
        return ESP_ERR_NO_MEM;
    }
    for (const MapLayer &layer : kMapLayers) {
        if (!directory_exists(layer.root)) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", layer.id);
        cJSON_AddStringToObject(item, "name", layer.name);
        char url[80]{};
        std::snprintf(url, sizeof(url), "/tiles/%s/{z}/{x}/{y}.jpg", layer.id);
        cJSON_AddStringToObject(item, "url", url);
        cJSON_AddNumberToObject(item, "minZoom", layer.minimum_zoom);
        cJSON_AddNumberToObject(item, "maxZoom", layer.maximum_zoom);
        cJSON_AddItemToArray(layers, item);
    }
    const esp_err_t result = send_json(request, response);
    cJSON_Delete(response);
    return result;
}

esp_err_t unavailable_handler(httpd_req_t *request)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", "daughterboard_radio_unavailable");
    cJSON_AddStringToObject(json, "detail",
                            "The P4 local dashboard is active; collar radio commands require the SX1262 daughterboard.");
    const esp_err_t result = send_json(request, json, "503 Service Unavailable");
    cJSON_Delete(json);
    return result;
}

esp_err_t hub_mode_handler(httpd_req_t *request)
{
    if (request->content_len == 0 || request->content_len >= 96) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid mode request");
    }
    char body[96]{};
    std::size_t received = 0;
    while (received < request->content_len) {
        const int count = httpd_req_recv(request, body + received,
                                         request->content_len - received);
        if (count <= 0) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                                   "Could not read request");
        received += static_cast<std::size_t>(count);
    }
    body[received] = '\0';

    char requested[16]{};
    if (httpd_query_key_value(body, "mode", requested, sizeof(requested)) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Missing hub mode");
    }
    hub::CommunicationsMode mode{};
    if (std::strcmp(requested, "home") == 0) mode = hub::CommunicationsMode::Home;
    else if (std::strcmp(requested, "portable") == 0) mode = hub::CommunicationsMode::Portable;
    else if (std::strcmp(requested, "off_grid") == 0 ||
             std::strcmp(requested, "off-grid") == 0) mode = hub::CommunicationsMode::OffGrid;
    else return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Unknown hub mode");

    const WebSnapshot state = snapshot();
    if (state.cloud.effective_mode == hub::CommunicationsMode::OffGrid &&
        mode != hub::CommunicationsMode::OffGrid) {
        char confirmation[8]{};
        if (httpd_query_key_value(body, "confirm", confirmation, sizeof(confirmation)) != ESP_OK ||
            std::strcmp(confirmation, "true") != 0) {
            cJSON *json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "error", "confirmation_required");
            const esp_err_t result = send_json(request, json, "409 Conflict");
            cJSON_Delete(json);
            return result;
        }
    }

    int expected = -1;
    if (!g_requested_mode.compare_exchange_strong(expected, static_cast<int>(mode))) {
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "error", "mode_change_pending");
        const esp_err_t result = send_json(request, json, "409 Conflict");
        cJSON_Delete(json);
        return result;
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "pending", true);
    const esp_err_t result = send_json(request, json, "202 Accepted");
    cJSON_Delete(json);
    return result;
}

esp_err_t history_handler(httpd_req_t *request)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", "journal_unavailable");
    cJSON_AddStringToObject(json, "detail", "P4 journal migration is not complete.");
    const esp_err_t result = send_json(request, json, "501 Not Implemented");
    cJSON_Delete(json);
    return result;
}

const char *content_type(const char *path)
{
    const char *extension = std::strrchr(path, '.');
    if (extension == nullptr) return "application/octet-stream";
    if (std::strcmp(extension, ".html") == 0) return "text/html";
    if (std::strcmp(extension, ".css") == 0) return "text/css";
    if (std::strcmp(extension, ".js") == 0) return "application/javascript";
    if (std::strcmp(extension, ".json") == 0) return "application/json";
    if (std::strcmp(extension, ".png") == 0) return "image/png";
    if (std::strcmp(extension, ".svg") == 0) return "image/svg+xml";
    if (std::strcmp(extension, ".ico") == 0) return "image/x-icon";
    if (std::strcmp(extension, ".avif") == 0) return "image/avif";
    return "application/octet-stream";
}

bool public_path(const char *uri)
{
    constexpr const char *allowed[] = {
        "/index.html", "/welcome.html", "/style.css", "/app.js", "/welcome.js",
        "/leaflet.js", "/leaflet.css", "/basemap.json", "/feedback.js",
        "/hub-presence.js", "/hub-presence.css", "/favicon.svg", "/brand-favicon.ico",
        "/brand-mascot.avif", "/location-fit-markers.png", "/map-location.png",
        "/map-layers.png",
        "/images/marker-icon.png", "/images/marker-icon-2x.png", "/images/marker-shadow.png",
    };
    for (const char *candidate : allowed) {
        if (std::strcmp(uri, candidate) == 0) return true;
    }
    return false;
}

esp_err_t serve_file(httpd_req_t *request, const char *uri)
{
    char path[192]{};
    std::snprintf(path, sizeof(path), "%s%s", kWebRoot, uri);
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
    httpd_resp_set_type(request, content_type(uri));
    const char *extension = std::strrchr(uri, '.');
    const bool live_ui_asset = extension != nullptr &&
        (std::strcmp(extension, ".html") == 0 ||
         std::strcmp(extension, ".css") == 0 ||
         std::strcmp(extension, ".js") == 0);
    // The hub UI is flashed frequently during development. Never let an old
    // stylesheet survive a firmware update and make new markup look broken.
    httpd_resp_set_hdr(request, "Cache-Control",
                       live_ui_asset ? "no-store" : "public, max-age=86400");
    char buffer[2048];
    std::size_t count = 0;
    esp_err_t result = ESP_OK;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        result = httpd_resp_send_chunk(request, buffer, count);
        if (result != ESP_OK) break;
    }
    std::fclose(file);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, nullptr, 0);
    return result;
}

esp_err_t serve_map_tile(httpd_req_t *request)
{
    char layer_id[16]{};
    int zoom = -1;
    int x = -1;
    int y = -1;
    int consumed = 0;
    if (std::sscanf(request->uri, "/tiles/%15[^/]/%d/%d/%d.jpg%n",
                    layer_id, &zoom, &x, &y, &consumed) != 4 ||
        consumed <= 0 || request->uri[consumed] != '\0' || x < 0 || y < 0) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid tile path");
    }

    const MapLayer *selected = nullptr;
    for (const MapLayer &layer : kMapLayers) {
        if (std::strcmp(layer.id, layer_id) == 0) {
            selected = &layer;
            break;
        }
    }
    if (selected == nullptr || zoom < selected->minimum_zoom ||
        zoom > selected->maximum_zoom || !directory_exists(selected->root)) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Tile unavailable");
    }

    char path[256]{};
    std::snprintf(path, sizeof(path), "%s/%d/%d/%d.jpg", selected->root, zoom, x, y);
    FILE *file = std::fopen(path, "rb");
    if (file == nullptr) return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Tile unavailable");
    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=86400");
    char buffer[4096];
    esp_err_t result = ESP_OK;
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        result = httpd_resp_send_chunk(request, buffer, count);
        if (result != ESP_OK) break;
    }
    std::fclose(file);
    if (result == ESP_OK) result = httpd_resp_send_chunk(request, nullptr, 0);
    return result;
}

esp_err_t root_handler(httpd_req_t *request)
{
    return serve_file(request, "/index.html");
}

esp_err_t welcome_handler(httpd_req_t *request)
{
    return serve_file(request, "/welcome.html");
}

esp_err_t events_handler(httpd_req_t *request)
{
    // HTTP 204 tells EventSource not to reconnect. The P4 dashboard deliberately
    // uses its five-second REST confidence poll until native SSE is implemented;
    // a 503 made every open browser tab reconnect forever and churn sockets.
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t captive_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://bluepaws.local/welcome");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, nullptr, 0);
}

esp_err_t wildcard_handler(httpd_req_t *request)
{
    // HTTPD leaves the query string in request->uri. Strip it before matching
    // static files so cache-busting URLs such as style.css?v=... still resolve.
    char uri[192]{};
    const char *query = std::strchr(request->uri, '?');
    const std::size_t length = query == nullptr
        ? std::strlen(request->uri)
        : static_cast<std::size_t>(query - request->uri);
    if (length >= sizeof(uri)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "URI too long");
    }
    std::memcpy(uri, request->uri, length);
    uri[length] = '\0';
    if (public_path(uri)) return serve_file(request, uri);
    if (std::strncmp(uri, "/tiles/", 7) == 0) return serve_map_tile(request);
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
}

bool register_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    const httpd_uri_t route{
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = nullptr,
    };
    return httpd_register_uri_handler(g_server, &route) == ESP_OK;
}

}  // namespace

bool start_server_now()
{
    if (g_server != nullptr) return true;

    const esp_vfs_spiffs_conf_t filesystem{
        .base_path = kWebRoot,
        .partition_label = "storage",
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    const esp_err_t mount_result = esp_vfs_spiffs_register(&filesystem);
    if (mount_result != ESP_OK && mount_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Could not mount dashboard SPIFFS: %s", esp_err_to_name(mount_result));
        return false;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    // Modern browsers open several asset connections in parallel. Leave room
    // for two local dashboard tabs while LRU purging protects the AP server.
    config.max_open_sockets = 12;
    config.backlog_conn = 8;
    config.max_uri_handlers = 32;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&g_server, &config) != ESP_OK) {
        ESP_LOGE(kTag, "Could not start local HTTP server");
        g_server = nullptr;
        return false;
    }

    bool ok = true;
    ok &= register_uri("/", HTTP_GET, root_handler);
    ok &= register_uri("/welcome", HTTP_GET, welcome_handler);
    ok &= register_uri("/api/welcome", HTTP_GET, welcome_api_handler);
    ok &= register_uri("/api/devices", HTTP_GET, devices_handler);
    ok &= register_uri("/api/status", HTTP_GET, status_handler);
    ok &= register_uri("/api/hub-presence", HTTP_GET, hub_presence_handler);
    ok &= register_uri("/api/map-layers", HTTP_GET, map_layers_handler);
    ok &= register_uri("/api/commands", HTTP_GET, empty_array_handler);
    ok &= register_uri("/api/ble", HTTP_GET, empty_array_handler);
    ok &= register_uri("/api/history*", HTTP_GET, history_handler);
    ok &= register_uri("/events", HTTP_GET, events_handler);
    ok &= register_uri("/api/command", HTTP_POST, unavailable_handler);
    ok &= register_uri("/api/find", HTTP_POST, unavailable_handler);
    ok &= register_uri("/api/device-status", HTTP_POST, unavailable_handler);
    ok &= register_uri("/api/hub-mode", HTTP_POST, hub_mode_handler);
    ok &= register_uri("/api/config", HTTP_POST, unavailable_handler);
    ok &= register_uri("/api/hub-preferences", HTTP_POST, unavailable_handler);
    ok &= register_uri("/api/device-meta", HTTP_POST, unavailable_handler);
    ok &= register_uri("/api/security*", HTTP_GET, unavailable_handler);
    ok &= register_uri("/api/security*", HTTP_POST, unavailable_handler);
    ok &= register_uri("/generate_204", HTTP_GET, captive_handler);
    ok &= register_uri("/gen_204", HTTP_GET, captive_handler);
    ok &= register_uri("/hotspot-detect.html", HTTP_GET, captive_handler);
    ok &= register_uri("/library/test/success.html", HTTP_GET, captive_handler);
    ok &= register_uri("/ncsi.txt", HTTP_GET, captive_handler);
    ok &= register_uri("/connecttest.txt", HTTP_GET, captive_handler);
    ok &= register_uri("/redirect", HTTP_GET, captive_handler);
    ok &= register_uri("/fwlink", HTTP_GET, captive_handler);
    ok &= register_uri("/*", HTTP_GET, wildcard_handler);
    ESP_LOGI(kTag, "P4 local dashboard listening on port 80 (%s)", ok ? "ready" : "partial");
    return ok;
}

bool start()
{
    if (g_server != nullptr || g_starting) return true;
    if (g_lock == nullptr) g_lock = xSemaphoreCreateMutex();
    if (g_lock == nullptr) return false;

    // SPIFFS registration scans the large storage partition and can occupy the
    // calling core for several seconds. Keep it away from app_main so CPU0's
    // watched idle task can run while the dashboard comes online on CPU1.
    g_starting = true;
    const BaseType_t created = xTaskCreatePinnedToCore(
        [](void *) {
            if (!start_server_now()) ESP_LOGE(kTag, "Local dashboard initialization failed");
            g_starting = false;
            vTaskDelete(nullptr);
        },
        "p4_web_start", 6144, nullptr, 2, nullptr, 1);
    if (created != pdPASS) {
        g_starting = false;
        ESP_LOGE(kTag, "Could not create local dashboard startup task");
        return false;
    }
    return true;
}

bool takeRequestedMode(hub::CommunicationsMode &mode)
{
    const int requested = g_requested_mode.exchange(-1);
    if (requested < static_cast<int>(hub::CommunicationsMode::Home) ||
        requested > static_cast<int>(hub::CommunicationsMode::OffGrid)) return false;
    mode = static_cast<hub::CommunicationsMode>(requested);
    return true;
}

void updateSnapshot(const CatStore &cats, const cloud::Status &cloud_status)
{
    if (g_lock == nullptr || xSemaphoreTake(g_lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    g_snapshot = {};
    g_snapshot.count = cats.size();
    for (std::size_t i = 0; i < g_snapshot.count; ++i) {
        const CatRecord *cat = cats.at(i);
        if (cat != nullptr) g_snapshot.cats[i] = *cat;
    }
    g_snapshot.cloud = cloud_status;
    xSemaphoreGive(g_lock);
}

}  // namespace bluepaws::web
