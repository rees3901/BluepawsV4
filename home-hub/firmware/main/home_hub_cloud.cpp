#include "home_hub_cloud.h"

#if __has_include("home_hub_secrets.h")
#include "home_hub_secrets.h"
#endif
#include "home_hub_config.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>

#if !defined(HOME_HUB_GATEWAY_TOKEN) && defined(CLOUD_BEARER_TOKEN)
#define HOME_HUB_GATEWAY_TOKEN CLOUD_BEARER_TOKEN
#endif
#ifndef HOME_HUB_GATEWAY_TOKEN
#define HOME_HUB_GATEWAY_TOKEN ""
#endif

namespace bluepaws::cloud {
namespace {

constexpr char kTag[] = "home_hub_cloud";
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kReconfigureBit = BIT1;
constexpr std::size_t kResponseBytes = 64U * 1024U;
constexpr char kStateDirectory[] = "/sdcard/bluepaws/data/state-v1";
constexpr char kAvatarDirectory[] = "/sdcard/bluepaws/data/avatars-v1";
constexpr char kSnapshotPath[] = "/sdcard/bluepaws/data/state-v1/latest.json";
constexpr char kSnapshotTemporaryPath[] = "/sdcard/bluepaws/data/state-v1/latest.tmp";

struct CloudUpdate {
    CatTelemetry telemetry{};
    char name[kCatNameBytes]{};
    char emoji[kAvatarEmojiBytes]{};
    char marker_colour[kMarkerColourBytes]{};
    bool photo_available = false;
};

struct HttpBuffer {
    char *data = nullptr;
    std::size_t length = 0;
    bool overflow = false;
};

QueueHandle_t g_updates = nullptr;
EventGroupHandle_t g_wifi = nullptr;
Status g_status{};
portMUX_TYPE g_status_lock = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_settings_lock = portMUX_INITIALIZER_UNLOCKED;
hub::Settings g_network_settings{};
bool g_wifi_initialized = false;
bool g_cloud_authorized = false;

uint32_t uptime_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void set_state(ConnectionState state) {
    portENTER_CRITICAL(&g_status_lock);
    g_status.state = state;
    portEXIT_CRITICAL(&g_status_lock);
}

void note_result(bool success, uint32_t http_status) {
    portENTER_CRITICAL(&g_status_lock);
    g_status.last_http_status = http_status;
    if (success) {
        ++g_status.successful_snapshots;
        g_status.last_sync_uptime_ms = uptime_ms();
        g_status.state = ConnectionState::Online;
    } else {
        ++g_status.failed_snapshots;
        g_status.state = ConnectionState::Degraded;
    }
    portEXIT_CRITICAL(&g_status_lock);
}

// Howard Hinnant's civil-date conversion, reduced to the UTC subset needed by
// Supabase ISO-8601 timestamps. It avoids timezone-dependent mktime().
int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153U * (month + (month > 2 ? -3 : 9)) + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

uint32_t parse_timestamp(const cJSON *value) {
    if (!cJSON_IsString(value) || value->valuestring == nullptr) return 0;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(value->valuestring, "%d-%d-%dT%d:%d:%d",
                    &year, &month, &day, &hour, &minute, &second) != 6) return 0;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60) return 0;
    const int64_t epoch = days_from_civil(year, static_cast<unsigned>(month),
                                          static_cast<unsigned>(day)) * 86400 +
                          hour * 3600 + minute * 60 + std::min(second, 59);
    return epoch > 0 && epoch <= std::numeric_limits<uint32_t>::max()
        ? static_cast<uint32_t>(epoch) : 0;
}

const cJSON *find_by_id(const cJSON *array, const char *field, int id) {
    if (!cJSON_IsArray(array)) return nullptr;
    const cJSON *row = nullptr;
    cJSON_ArrayForEach(row, array) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(row, field);
        if (cJSON_IsNumber(value) && value->valueint == id) return row;
    }
    return nullptr;
}

int number_or(const cJSON *object, const char *field, int fallback) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, field);
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}

double real_or(const cJSON *object, const char *field, double fallback) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, field);
    return cJSON_IsNumber(value) ? value->valuedouble : fallback;
}

const char *string_or(const cJSON *object, const char *field, const char *fallback) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, field);
    return cJSON_IsString(value) && value->valuestring != nullptr ? value->valuestring : fallback;
}

bool parse_snapshot(const char *json, std::size_t length) {
    cJSON *root = cJSON_ParseWithLength(json, length);
    if (root == nullptr) return false;
    const cJSON *latest = cJSON_GetObjectItemCaseSensitive(root, "latest");
    const cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    const cJSON *appearances = cJSON_GetObjectItemCaseSensitive(root, "appearances");
    if (!cJSON_IsArray(latest)) {
        cJSON_Delete(root);
        return false;
    }

    bool valid = true;
    const cJSON *position = nullptr;
    cJSON_ArrayForEach(position, latest) {
        const int device_id = number_or(position, "device_uid", 0);
        if (device_id <= 0 || device_id > 65535) continue;
        CloudUpdate update{};
        update.telemetry.device_id = static_cast<uint16_t>(device_id);
        update.telemetry.sequence = static_cast<uint32_t>(
            std::max(0, number_or(position, "message_id", 0)));
        update.telemetry.revision = static_cast<uint64_t>(std::max(
            0.0, real_or(position, "position_id", real_or(position, "observation_id", 0.0))));
        update.telemetry.latitude_e7 = static_cast<int32_t>(std::llround(
            real_or(position, "latitude", 0.0) * 1.0e7));
        update.telemetry.longitude_e7 = static_cast<int32_t>(std::llround(
            real_or(position, "longitude", 0.0) * 1.0e7));
        update.telemetry.battery_percent = static_cast<uint8_t>(std::clamp(
            number_or(position, "battery", 0), 0, 100));
        update.telemetry.battery_mv = static_cast<uint16_t>(std::clamp(
            number_or(position, "battery_mv", 0), 0, 65535));
        update.telemetry.rssi = static_cast<int16_t>(std::clamp(
            number_or(position, "link_rssi_dbm", -127), -32768, 32767));
        update.telemetry.snr = static_cast<float>(real_or(position, "link_snr_db", 0.0));
        update.telemetry.observed_at = parse_timestamp(
            cJSON_GetObjectItemCaseSensitive(position, "recorded_at"));
        update.telemetry.received_at_ms = uptime_ms();
        update.telemetry.position_valid =
            std::abs(real_or(position, "latitude", 0.0)) <= 90.0 &&
            std::abs(real_or(position, "longitude", 0.0)) <= 180.0;
        update.telemetry.source = TelemetrySource::Cloud;
        update.telemetry.status_code = static_cast<uint8_t>(std::clamp(
            number_or(position, "status_code", 1), 0, 3));
        update.telemetry.power_profile_code = static_cast<uint8_t>(std::clamp(
            number_or(position, "power_profile_code", 1), 0, 4));
        update.telemetry.flags = static_cast<uint8_t>(std::clamp(
            number_or(position, "flags", 0), 0, 255));
        update.telemetry.tx_reason = static_cast<uint8_t>(std::clamp(
            number_or(position, "tx_reason", 0), 0, 255));

        const cJSON *device = find_by_id(devices, "device_id", device_id);
        if (device != nullptr) {
            std::strncpy(update.name, string_or(device, "display_name", ""),
                         sizeof(update.name) - 1);
            const uint32_t presence_at = parse_timestamp(
                cJSON_GetObjectItemCaseSensitive(device, "last_seen_at"));
            if (presence_at > update.telemetry.observed_at) {
                update.telemetry.observed_at = presence_at;
                update.telemetry.status_code = static_cast<uint8_t>(std::clamp(
                    number_or(device, "last_seen_status_code", update.telemetry.status_code), 0, 3));
                update.telemetry.power_profile_code = static_cast<uint8_t>(std::clamp(
                    number_or(device, "last_seen_power_profile_code", update.telemetry.power_profile_code), 0, 4));
                update.telemetry.tx_reason = static_cast<uint8_t>(std::clamp(
                    number_or(device, "last_seen_tx_reason", update.telemetry.tx_reason), 0, 255));
                update.telemetry.battery_mv = static_cast<uint16_t>(std::clamp(
                    number_or(device, "last_seen_battery_mv", update.telemetry.battery_mv), 0, 65535));
            }
        }
        const cJSON *appearance = find_by_id(appearances, "device_id", device_id);
        if (appearance != nullptr) {
            std::strncpy(update.emoji, string_or(appearance, "emoji_value", ""),
                         sizeof(update.emoji) - 1);
            std::strncpy(update.marker_colour,
                         string_or(appearance, "marker_colour", ""),
                         sizeof(update.marker_colour) - 1);
            update.photo_available = std::strcmp(
                string_or(appearance, "avatar_kind", "emoji"), "photo") == 0;
        }
        if (xQueueSend(g_updates, &update, pdMS_TO_TICKS(50)) != pdTRUE) {
            valid = false;
            ESP_LOGW(kTag, "Cloud update queue full");
            break;
        }
    }
    cJSON_Delete(root);
    return valid;
}

void make_directory(const char *path) {
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(kTag, "Could not create %s: errno=%d", path, errno);
    }
}

void ensure_data_directories() {
    make_directory("/sdcard/bluepaws/data");
    make_directory(kStateDirectory);
    make_directory(kAvatarDirectory);
}

void cache_snapshot(const char *json, std::size_t length) {
    ensure_data_directories();
    FILE *file = std::fopen(kSnapshotTemporaryPath, "wb");
    if (file == nullptr) return;
    const bool written = std::fwrite(json, 1, length, file) == length;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::remove(kSnapshotTemporaryPath);
        return;
    }
    std::remove(kSnapshotPath);
    if (std::rename(kSnapshotTemporaryPath, kSnapshotPath) != 0) {
        ESP_LOGW(kTag, "Could not publish cached snapshot: errno=%d", errno);
    }
}

void restore_cached_snapshot() {
    FILE *file = std::fopen(kSnapshotPath, "rb");
    if (file == nullptr) return;
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return;
    }
    const long size = std::ftell(file);
    std::rewind(file);
    if (size <= 0 || static_cast<std::size_t>(size) >= kResponseBytes) {
        std::fclose(file);
        return;
    }
    auto *json = static_cast<char *>(std::malloc(static_cast<std::size_t>(size) + 1));
    if (json == nullptr) {
        std::fclose(file);
        return;
    }
    const std::size_t read = std::fread(json, 1, static_cast<std::size_t>(size), file);
    std::fclose(file);
    json[read] = '\0';
    if (read == static_cast<std::size_t>(size) && parse_snapshot(json, read)) {
        ESP_LOGI(kTag, "Restored last authoritative snapshot from SD");
    }
    std::free(json);
}

esp_err_t http_event(esp_http_client_event_t *event) {
    auto *buffer = static_cast<HttpBuffer *>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        const std::size_t incoming = static_cast<std::size_t>(event->data_len);
        if (buffer->length + incoming >= kResponseBytes) {
            buffer->overflow = true;
            return ESP_FAIL;
        }
        std::memcpy(buffer->data + buffer->length, event->data, incoming);
        buffer->length += incoming;
        buffer->data[buffer->length] = '\0';
    }
    return ESP_OK;
}

bool fetch_snapshot() {
    auto *storage = static_cast<char *>(heap_caps_malloc(kResponseBytes, MALLOC_CAP_SPIRAM));
    if (storage == nullptr) storage = static_cast<char *>(std::malloc(kResponseBytes));
    if (storage == nullptr) return false;
    HttpBuffer buffer{storage, 0, false};
    esp_http_client_config_t config{};
    config.url = HOME_HUB_SNAPSHOT_URL;
    config.event_handler = http_event;
    config.user_data = &buffer;
    config.timeout_ms = 12000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 4096;
    config.buffer_size_tx = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        std::free(storage);
        return false;
    }
    char authorization[256]{};
    std::snprintf(authorization, sizeof(authorization), "Bearer %s", HOME_HUB_GATEWAY_TOKEN);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Accept", "application/json");
    const esp_err_t result = esp_http_client_perform(client);
    const int status_code = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    const bool success = result == ESP_OK && status_code == 200 && !buffer.overflow &&
                         parse_snapshot(buffer.data, buffer.length);
    if (success) {
        cache_snapshot(buffer.data, buffer.length);
        ESP_LOGI(kTag, "Authoritative snapshot applied: http=%d bytes=%u",
                 status_code, static_cast<unsigned>(buffer.length));
    }
    esp_http_client_cleanup(client);
    std::memset(authorization, 0, sizeof(authorization));
    std::free(storage);
    note_result(success, static_cast<uint32_t>(std::max(0, status_code)));
    if (!success) ESP_LOGW(kTag, "Snapshot failed: transport=%s http=%d bytes=%u overflow=%d",
                           esp_err_to_name(result), status_code,
                           static_cast<unsigned>(buffer.length), buffer.overflow);
    return success;
}

void wifi_event(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi, kConnectedBit);
        set_state(ConnectionState::Connecting);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(g_wifi, kConnectedBit);
        set_state(ConnectionState::Online);
    }
}

hub::Settings network_settings() {
    portENTER_CRITICAL(&g_settings_lock);
    const hub::Settings copy = g_network_settings;
    portEXIT_CRITICAL(&g_settings_lock);
    return copy;
}

bool configure_wifi(const hub::Settings &settings, unsigned network_index, bool restart) {
    const hub::WifiNetwork &requested = network_index == 1
        ? settings.secondary : settings.primary;
    const hub::WifiNetwork &station = hub::validSsid(requested.ssid)
        ? requested
        : settings.primary;
    const bool station_enabled = hub::validSsid(station.ssid);
    const bool access_point_enabled = settings.access_point_enabled &&
        hub::validSsid(settings.access_point_ssid);
    if (!station_enabled && !access_point_enabled) return false;

    if (restart && g_wifi_initialized) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    const wifi_mode_t mode = station_enabled && access_point_enabled
        ? WIFI_MODE_APSTA
        : (station_enabled ? WIFI_MODE_STA : WIFI_MODE_AP);
    if (esp_wifi_set_mode(mode) != ESP_OK) return false;

    if (station_enabled) {
        wifi_config_t station_config{};
        const size_t ssid_length = std::strlen(station.ssid);
        const size_t password_length = std::strlen(station.password);
        std::memcpy(station_config.sta.ssid, station.ssid, ssid_length);
        std::memcpy(station_config.sta.password, station.password, password_length);
        station_config.sta.threshold.authmode = std::strlen(station.password) == 0
            ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        station_config.sta.pmf_cfg.capable = true;
        station_config.sta.pmf_cfg.required = false;
        if (esp_wifi_set_config(WIFI_IF_STA, &station_config) != ESP_OK) return false;
    }
    if (access_point_enabled) {
        wifi_config_t access_point_config{};
        const size_t ssid_length = std::strlen(settings.access_point_ssid);
        const size_t password_length = std::strlen(settings.access_point_password);
        std::memcpy(access_point_config.ap.ssid, settings.access_point_ssid, ssid_length);
        std::memcpy(access_point_config.ap.password, settings.access_point_password,
                    password_length);
        access_point_config.ap.ssid_len = ssid_length;
        access_point_config.ap.channel = 6;
        access_point_config.ap.max_connection = 4;
        access_point_config.ap.authmode =
            std::strlen(settings.access_point_password) == 0
                ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        if (esp_wifi_set_config(WIFI_IF_AP, &access_point_config) != ESP_OK) return false;
    }
    const bool started = esp_wifi_start() == ESP_OK;
    if (started) {
        g_wifi_initialized = true;
        ESP_LOGI(kTag, "Wi-Fi applied: station=%s fallback_ap=%s",
                 station_enabled ? station.ssid : "disabled",
                 access_point_enabled ? settings.access_point_ssid : "disabled");
    }
    return started;
}

bool start_wifi(const hub::Settings &settings) {
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    if (error != ESP_OK) return false;
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) return false;
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr);
    return configure_wifi(settings, 0, false);
}

void sync_task(void *) {
    set_state(ConnectionState::Starting);
    hub::Settings settings = network_settings();
    if (!start_wifi(settings)) {
        ESP_LOGE(kTag, "ESP-Hosted Wi-Fi initialization failed");
        set_state(ConnectionState::Degraded);
        vTaskDelete(nullptr);
        return;
    }
    uint32_t delay_ms = HOME_HUB_SYNC_INTERVAL_MS;
    unsigned network_index = 0;
    while (true) {
        const EventBits_t connected = xEventGroupWaitBits(
            g_wifi,
            kConnectedBit | kReconfigureBit,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(30000));
        if ((connected & kReconfigureBit) != 0) {
            xEventGroupClearBits(g_wifi, kReconfigureBit | kConnectedBit);
            settings = network_settings();
            network_index = 0;
            configure_wifi(settings, network_index, true);
            delay_ms = HOME_HUB_SYNC_INTERVAL_MS;
            continue;
        }
        if ((connected & kConnectedBit) == 0) {
            set_state(ConnectionState::Connecting);
            if (hub::validSsid(settings.secondary.ssid)) {
                network_index = network_index == 0 ? 1 : 0;
                configure_wifi(settings, network_index, true);
            }
            delay_ms = std::min(delay_ms * 2U,
                                static_cast<uint32_t>(HOME_HUB_SYNC_MAX_BACKOFF_MS));
        } else if (!g_cloud_authorized || fetch_snapshot()) {
            delay_ms = HOME_HUB_SYNC_INTERVAL_MS;
        } else {
            delay_ms = std::min(delay_ms * 2U,
                                static_cast<uint32_t>(HOME_HUB_SYNC_MAX_BACKOFF_MS));
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

}  // namespace

bool start(const hub::Settings &settings) {
    g_cloud_authorized = std::strlen(HOME_HUB_GATEWAY_TOKEN) >= 16;
    if (!g_cloud_authorized) {
        ESP_LOGW(kTag, "Cloud sync disabled: home_hub_secrets.h is not provisioned");
        set_state(ConnectionState::Disabled);
    }
    portENTER_CRITICAL(&g_settings_lock);
    g_network_settings = settings;
    portEXIT_CRITICAL(&g_settings_lock);
    g_updates = xQueueCreate(kMaximumCats * 2, sizeof(CloudUpdate));
    g_wifi = xEventGroupCreate();
    if (g_updates == nullptr || g_wifi == nullptr) return false;
    if (g_cloud_authorized) restore_cached_snapshot();
    const bool started = xTaskCreate(sync_task, "hub_cloud", 12288, nullptr, 5, nullptr) == pdPASS;
    return started && g_cloud_authorized;
}

bool applyNetworkSettings(const hub::Settings &input) {
    hub::Settings settings = input;
    hub::sanitize(settings);
    portENTER_CRITICAL(&g_settings_lock);
    g_network_settings = settings;
    portEXIT_CRITICAL(&g_settings_lock);
    if (g_wifi != nullptr) xEventGroupSetBits(g_wifi, kReconfigureBit);
    return true;
}

std::size_t drain(CatStore &store) {
    if (g_updates == nullptr) return 0;
    std::size_t count = 0;
    CloudUpdate update{};
    while (xQueueReceive(g_updates, &update, 0) == pdTRUE) {
        const ApplyResult result = store.apply(update.telemetry);
        if (result != ApplyResult::InvalidDevice && result != ApplyResult::CapacityReached) {
            if (update.name[0] != '\0') store.setName(update.telemetry.device_id, update.name);
            store.setAppearance(update.telemetry.device_id, update.emoji,
                                update.marker_colour, update.photo_available);
            ++count;
        }
    }
    return count;
}

Status status() {
    portENTER_CRITICAL(&g_status_lock);
    const Status copy = g_status;
    portEXIT_CRITICAL(&g_status_lock);
    return copy;
}

}  // namespace bluepaws::cloud
