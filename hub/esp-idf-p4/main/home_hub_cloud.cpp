#include "home_hub_cloud.h"

#if __has_include("home_hub_secrets.h")
#include "home_hub_secrets.h"
#endif
#include "home_hub_config.h"
#include "home_hub_bluetooth.h"
#include "home_hub_mdns.h"
#include "home_hub_testbed_telemetry.h"
#include "bluepaws/hub_mode_policy.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs_flash.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

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
constexpr EventBits_t kDisconnectedBit = BIT2;
constexpr EventBits_t kWifiScanBit = BIT3;
constexpr uint64_t kStationReconnectDelayUs = 5ULL * 1000ULL * 1000ULL;
constexpr std::size_t kResponseBytes = 64U * 1024U;
constexpr std::size_t kPresenceResponseBytes = 4096U;
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
    std::size_t capacity = 0;
    std::size_t length = 0;
    bool overflow = false;
};

QueueHandle_t g_updates = nullptr;
QueueHandle_t g_controls = nullptr;
EventGroupHandle_t g_wifi = nullptr;
Status g_status{};
portMUX_TYPE g_status_lock = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_settings_lock = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE g_wifi_scan_lock = portMUX_INITIALIZER_UNLOCKED;
// Keep the network task's full settings copy in PSRAM. Internal DRAM is tight
// before FreeRTOS creates app_main, whereas this state is accessed only during
// periodic control/network cycles.
hub::Settings *g_network_settings = nullptr;
// The application is already close to the ESP32-P4 internal DRAM limit before
// app_main starts. Keep scan results in PSRAM so adding the picker cannot stop
// FreeRTOS from allocating the main task.
WifiScanSnapshot *g_wifi_scan = nullptr;
bool g_wifi_initialized = false;
bool g_cloud_authorized = false;
std::atomic_bool g_station_allowed{false};
esp_timer_handle_t g_reconnect_timer = nullptr;
uint32_t g_last_self_report_ms = 0;
uint32_t g_self_report_retry_ms = HOME_HUB_SYNC_INTERVAL_MS;
bool g_self_reported = false;
std::atomic_bool g_force_self_report{false};
std::atomic_bool g_force_control_poll{true};
std::atomic_bool g_control_pending{false};
uint32_t g_last_control_poll_ms = 0;
uint8_t g_control_poll_failures = 0;
bool g_control_poll_started = false;

void time_sync_notification(struct timeval *) {
    portENTER_CRITICAL(&g_status_lock);
    g_status.time_synchronized = true;
    portEXIT_CRITICAL(&g_status_lock);
    ESP_LOGI(kTag, "System clock synchronized by NTP");
}

uint32_t uptime_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

void set_state(ConnectionState state) {
    portENTER_CRITICAL(&g_status_lock);
    g_status.state = state;
    portEXIT_CRITICAL(&g_status_lock);
}

void set_mode_status(hub::CommunicationsMode requested,
                     hub::CommunicationsMode effective,
                     ModeReason reason,
                     bool automatic_off_grid = false) {
    portENTER_CRITICAL(&g_status_lock);
    g_status.requested_mode = requested;
    g_status.effective_mode = effective;
    g_status.mode_reason = reason;
    g_status.automatic_off_grid = automatic_off_grid;
    portEXIT_CRITICAL(&g_status_lock);
}

hub::CommunicationsMode network_mode(unsigned network_index) {
    return hub::effectiveModeForNetwork(network_index);
}

ModeReason network_reason(unsigned network_index) {
    return network_index == 1 ? ModeReason::SecondaryWifi : ModeReason::PrimaryWifi;
}

void set_network_mode(const hub::Settings &settings, unsigned network_index) {
    set_mode_status(settings.communications_mode, network_mode(network_index),
                    network_reason(network_index));
}

void set_off_grid_mode(const hub::Settings &settings, bool automatic) {
    set_mode_status(settings.communications_mode, hub::CommunicationsMode::OffGrid,
                    automatic ? ModeReason::WifiUnavailable
                              : ModeReason::ManualSelection,
                    automatic);
}

void clear_station_link() {
    portENTER_CRITICAL(&g_status_lock);
    g_status.wifi_station_connected = false;
    g_status.wifi_rssi_dbm = -127;
    g_status.wifi_ssid[0] = '\0';
    portEXIT_CRITICAL(&g_status_lock);
}

void refresh_station_link() {
    wifi_ap_record_t access_point{};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        clear_station_link();
        return;
    }
    portENTER_CRITICAL(&g_status_lock);
    g_status.wifi_station_connected = true;
    g_status.wifi_rssi_dbm = access_point.rssi;
    std::strncpy(g_status.wifi_ssid,
                 reinterpret_cast<const char *>(access_point.ssid),
                 sizeof(g_status.wifi_ssid) - 1);
    g_status.wifi_ssid[sizeof(g_status.wifi_ssid) - 1] = '\0';
    portEXIT_CRITICAL(&g_status_lock);
}

void perform_wifi_scan() {
    if (g_wifi_scan == nullptr) return;
    wifi_mode_t original_mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&original_mode) != ESP_OK) {
        portENTER_CRITICAL(&g_wifi_scan_lock);
        g_wifi_scan->state = WifiScanState::Failed;
        portEXIT_CRITICAL(&g_wifi_scan_lock);
        return;
    }

    const bool add_station_interface = original_mode == WIFI_MODE_AP;
    if (add_station_interface && esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
        portENTER_CRITICAL(&g_wifi_scan_lock);
        g_wifi_scan->state = WifiScanState::Failed;
        portEXIT_CRITICAL(&g_wifi_scan_lock);
        return;
    }

    wifi_scan_config_t scan_config{};
    scan_config.show_hidden = false;
    const esp_err_t scan_error = esp_wifi_scan_start(&scan_config, true);
    std::array<wifi_ap_record_t, 24> access_points{};
    uint16_t found = static_cast<uint16_t>(access_points.size());
    esp_err_t results_error = scan_error;
    if (scan_error == ESP_OK) {
        results_error = esp_wifi_scan_get_ap_records(&found, access_points.data());
    }

    WifiScanSnapshot completed{};
    completed.state = results_error == ESP_OK ? WifiScanState::Ready
                                               : WifiScanState::Failed;
    portENTER_CRITICAL(&g_wifi_scan_lock);
    completed.generation = g_wifi_scan->generation;
    portEXIT_CRITICAL(&g_wifi_scan_lock);
    if (results_error == ESP_OK) {
        for (uint16_t i = 0; i < found && completed.count < completed.results.size(); ++i) {
            const char *ssid = reinterpret_cast<const char *>(access_points[i].ssid);
            if (ssid[0] == '\0') continue;
            bool duplicate = false;
            for (std::size_t existing = 0; existing < completed.count; ++existing) {
                if (std::strcmp(completed.results[existing].ssid, ssid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            WifiScanResult &result = completed.results[completed.count++];
            std::strncpy(result.ssid, ssid, sizeof(result.ssid) - 1);
            result.rssi_dbm = access_points[i].rssi;
            result.secured = access_points[i].authmode != WIFI_AUTH_OPEN;
        }
    }

    if (add_station_interface) {
        // Returning APSTA to AP leaves the local Off-Grid hotspot running and
        // ensures a settings scan cannot silently enable station operation.
        esp_wifi_set_mode(original_mode);
    }
    portENTER_CRITICAL(&g_wifi_scan_lock);
    *g_wifi_scan = completed;
    portEXIT_CRITICAL(&g_wifi_scan_lock);
    ESP_LOGI(kTag, "Wi-Fi scan %s with %u unique networks",
             completed.state == WifiScanState::Ready ? "completed" : "failed",
             static_cast<unsigned>(completed.count));
}

bool handle_wifi_scan(EventBits_t events) {
    if ((events & kWifiScanBit) == 0) return false;
    xEventGroupClearBits(g_wifi, kWifiScanBit);
    perform_wifi_scan();
    return true;
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

void note_self_report_result(bool success, uint32_t http_status) {
    portENTER_CRITICAL(&g_status_lock);
    g_status.last_self_report_http_status = http_status;
    if (success) {
        ++g_status.successful_self_reports;
        g_status.last_self_report_uptime_ms = uptime_ms();
    } else {
        ++g_status.failed_self_reports;
    }
    portEXIT_CRITICAL(&g_status_lock);
}

const char *cloud_mode_name(hub::CommunicationsMode mode) {
    switch (mode) {
    case hub::CommunicationsMode::Portable: return "portable";
    case hub::CommunicationsMode::OffGrid: return "off_grid";
    case hub::CommunicationsMode::Home: return "home";
    }
    return "home";
}

bool parse_reporting_profile(const char *value, hub::ReportingProfile &profile) {
    if (value == nullptr) return false;
    if (std::strcmp(value, "power_save") == 0) {
        profile = hub::ReportingProfile::PowerSave;
        return true;
    }
    if (std::strcmp(value, "normal") == 0) {
        profile = hub::ReportingProfile::Normal;
        return true;
    }
    if (std::strcmp(value, "active") == 0) {
        profile = hub::ReportingProfile::Active;
        return true;
    }
    return false;
}

bool copy_control_text(char *destination, std::size_t capacity, const cJSON *value) {
    if (destination == nullptr || capacity == 0 || !cJSON_IsString(value) ||
        value->valuestring == nullptr) return false;
    const std::size_t length = std::strlen(value->valuestring);
    if (length == 0 || length >= capacity) return false;
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(value->valuestring[index]);
        if (byte < 0x20U || byte == 0x7fU) return false;
    }
    std::memcpy(destination, value->valuestring, length + 1);
    return true;
}

bool valid_marker_colour(const char *value) {
    if (value == nullptr || std::strlen(value) != 7 || value[0] != '#') return false;
    for (std::size_t index = 1; index < 7; ++index) {
        const char c = value[index];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
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

TelemetryLink link_type(const cJSON *position) {
    const char *value = string_or(position, "link_type", "");
    if (std::strcmp(value, "lte") == 0) return TelemetryLink::Lte;
    if (std::strcmp(value, "lora") == 0) return TelemetryLink::LoRa;
    if (std::strcmp(value, "wifi") == 0) return TelemetryLink::Wifi;
    return TelemetryLink::Unknown;
}

void apply_cloud_metadata(CloudUpdate &update, const cJSON *device,
                          const cJSON *appearance) {
    if (device != nullptr) {
        std::strncpy(update.name, string_or(device, "display_name", ""),
                     sizeof(update.name) - 1);
    }
    if (appearance != nullptr) {
        std::strncpy(update.emoji, string_or(appearance, "emoji_value", ""),
                     sizeof(update.emoji) - 1);
        std::strncpy(update.marker_colour,
                     string_or(appearance, "marker_colour", ""),
                     sizeof(update.marker_colour) - 1);
        update.photo_available = std::strcmp(
            string_or(appearance, "avatar_kind", "emoji"), "photo") == 0;
    }
}

bool queue_cloud_update(const CloudUpdate &update) {
    if (xQueueSend(g_updates, &update, pdMS_TO_TICKS(50)) == pdTRUE) return true;
    ESP_LOGW(kTag, "Cloud update queue full");
    return false;
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
        const cJSON *latitude = cJSON_GetObjectItemCaseSensitive(position, "latitude");
        const cJSON *longitude = cJSON_GetObjectItemCaseSensitive(position, "longitude");
        const double latitude_value = cJSON_IsNumber(latitude) ? latitude->valuedouble : 0.0;
        const double longitude_value = cJSON_IsNumber(longitude) ? longitude->valuedouble : 0.0;
        update.telemetry.latitude_e7 = static_cast<int32_t>(std::llround(
            latitude_value * 1.0e7));
        update.telemetry.longitude_e7 = static_cast<int32_t>(std::llround(
            longitude_value * 1.0e7));
        update.telemetry.battery_percent = static_cast<uint8_t>(std::clamp(
            number_or(position, "battery", 0), 0, 100));
        update.telemetry.battery_mv = static_cast<uint16_t>(std::clamp(
            number_or(position, "battery_mv", 0), 0, 65535));
        update.telemetry.rssi = static_cast<int16_t>(std::clamp(
            number_or(position, "link_rssi_dbm", -127), -32768, 32767));
        update.telemetry.snr = static_cast<float>(real_or(position, "link_snr_db", 0.0));
        const uint32_t recorded_at = parse_timestamp(
            cJSON_GetObjectItemCaseSensitive(position, "recorded_at"));
        const uint32_t received_at = parse_timestamp(
            cJSON_GetObjectItemCaseSensitive(position, "received_at"));
        // Match the web dashboard: a collar clock ahead of the backend must
        // not freeze freshness at zero or defeat a later local observation.
        update.telemetry.observed_at = recorded_at != 0 && received_at != 0
            ? std::min(recorded_at, received_at)
            : std::max(recorded_at, received_at);
        update.telemetry.received_at_ms = uptime_ms();
        update.telemetry.position_valid = cJSON_IsNumber(latitude) &&
            cJSON_IsNumber(longitude) && std::abs(latitude_value) <= 90.0 &&
            std::abs(longitude_value) <= 180.0;
        update.telemetry.source = TelemetrySource::Cloud;
        update.telemetry.link = link_type(position);
        update.telemetry.status_code = static_cast<uint8_t>(std::clamp(
            number_or(position, "status_code", 1), 0, 3));
        update.telemetry.power_profile_code = static_cast<uint8_t>(std::clamp(
            number_or(position, "power_profile_code", 1), 0, 4));
        update.telemetry.flags = static_cast<uint8_t>(std::clamp(
            number_or(position, "flags", 0), 0, 255));
        update.telemetry.tx_reason = static_cast<uint8_t>(std::clamp(
            number_or(position, "tx_reason", 0), 0, 255));

        const cJSON *device = find_by_id(devices, "device_id", device_id);
        const cJSON *appearance = find_by_id(appearances, "device_id", device_id);
        apply_cloud_metadata(update, device, appearance);
        if (!queue_cloud_update(update)) {
            valid = false;
            break;
        }

        // Presence is activity, not a replacement position. Queue it as a
        // no-fix observation so a recent wake/check-in can update status and
        // battery without stamping an older coordinate with a newer time.
        const uint32_t presence_at = device == nullptr ? 0 : parse_timestamp(
            cJSON_GetObjectItemCaseSensitive(device, "last_seen_at"));
        if (presence_at > update.telemetry.observed_at) {
            CloudUpdate presence = update;
            presence.telemetry.observed_at = presence_at;
            presence.telemetry.position_valid = false;
            presence.telemetry.status_code = static_cast<uint8_t>(std::clamp(
                number_or(device, "last_seen_status_code", presence.telemetry.status_code), 0, 3));
            presence.telemetry.power_profile_code = static_cast<uint8_t>(std::clamp(
                number_or(device, "last_seen_power_profile_code", presence.telemetry.power_profile_code), 0, 4));
            presence.telemetry.tx_reason = static_cast<uint8_t>(std::clamp(
                number_or(device, "last_seen_tx_reason", presence.telemetry.tx_reason), 0, 255));
            presence.telemetry.battery_mv = static_cast<uint16_t>(std::clamp(
                number_or(device, "last_seen_battery_mv", presence.telemetry.battery_mv), 0, 65535));
            if (!queue_cloud_update(presence)) {
                valid = false;
                break;
            }
        }
    }

    // The web dashboard also shows affiliated collars that have checked in but
    // have never supplied a valid position. Preserve that roster parity.
    if (valid && cJSON_IsArray(devices)) {
        const cJSON *device = nullptr;
        cJSON_ArrayForEach(device, devices) {
            const int device_id = number_or(device, "device_id", 0);
            if (device_id <= 0 || device_id > 65535 ||
                find_by_id(latest, "device_uid", device_id) != nullptr) continue;
            const uint32_t presence_at = parse_timestamp(
                cJSON_GetObjectItemCaseSensitive(device, "last_seen_at"));
            if (presence_at == 0) continue;
            CloudUpdate presence{};
            presence.telemetry.device_id = static_cast<uint16_t>(device_id);
            presence.telemetry.observed_at = presence_at;
            presence.telemetry.received_at_ms = uptime_ms();
            presence.telemetry.position_valid = false;
            presence.telemetry.source = TelemetrySource::Cloud;
            presence.telemetry.status_code = static_cast<uint8_t>(std::clamp(
                number_or(device, "last_seen_status_code", 1), 0, 3));
            presence.telemetry.power_profile_code = static_cast<uint8_t>(std::clamp(
                number_or(device, "last_seen_power_profile_code", 1), 0, 4));
            presence.telemetry.tx_reason = static_cast<uint8_t>(std::clamp(
                number_or(device, "last_seen_tx_reason", 0), 0, 255));
            presence.telemetry.battery_mv = static_cast<uint16_t>(std::clamp(
                number_or(device, "last_seen_battery_mv", 0), 0, 65535));
            apply_cloud_metadata(presence, device,
                find_by_id(appearances, "device_id", device_id));
            if (!queue_cloud_update(presence)) {
                valid = false;
                break;
            }
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
        if (buffer->data == nullptr || buffer->capacity == 0 ||
            buffer->length + incoming >= buffer->capacity) {
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
    HttpBuffer buffer{storage, kResponseBytes, 0, false};
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

bool parse_control_settings(const char *json, std::size_t length) {
    cJSON *root = cJSON_ParseWithLength(json, length);
    if (root == nullptr) return false;
    const cJSON *settings = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (cJSON_IsNull(settings)) {
        cJSON_Delete(root);
        return true;
    }
    if (!cJSON_IsObject(settings)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *revision = cJSON_GetObjectItemCaseSensitive(settings, "revision");
    const cJSON *bluetooth = cJSON_GetObjectItemCaseSensitive(settings, "ble_enabled");
    ControlUpdate update{};
    const bool valid_revision = cJSON_IsNumber(revision) && revision->valuedouble >= 0.0 &&
        revision->valuedouble <= 9007199254740991.0;
    const bool valid = valid_revision && cJSON_IsBool(bluetooth) &&
        parse_reporting_profile(string_or(settings, "reporting_profile", nullptr),
                                update.reporting_profile) &&
        copy_control_text(update.display_name, sizeof(update.display_name),
                          cJSON_GetObjectItemCaseSensitive(settings, "display_name")) &&
        copy_control_text(update.home_emoji, sizeof(update.home_emoji),
                          cJSON_GetObjectItemCaseSensitive(settings, "home_emoji")) &&
        copy_control_text(update.portable_emoji, sizeof(update.portable_emoji),
                          cJSON_GetObjectItemCaseSensitive(settings, "portable_emoji")) &&
        copy_control_text(update.marker_colour, sizeof(update.marker_colour),
                          cJSON_GetObjectItemCaseSensitive(settings, "marker_colour")) &&
        valid_marker_colour(update.marker_colour);
    if (!valid) {
        cJSON_Delete(root);
        return false;
    }
    update.revision = static_cast<uint64_t>(revision->valuedouble);
    update.bluetooth_enabled = cJSON_IsTrue(bluetooth);

    uint64_t applied_revision = 0;
    portENTER_CRITICAL(&g_status_lock);
    applied_revision = g_status.applied_settings_revision;
    portEXIT_CRITICAL(&g_status_lock);
    if (update.revision > applied_revision && g_controls != nullptr) {
        g_control_pending.store(true);
        xQueueOverwrite(g_controls, &update);
    } else if (update.revision <= applied_revision) {
        g_control_pending.store(false);
    }
    cJSON_Delete(root);
    return true;
}

bool fetch_hub_settings() {
    auto *storage = static_cast<char *>(heap_caps_malloc(
        kPresenceResponseBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (storage == nullptr) storage = static_cast<char *>(std::malloc(kPresenceResponseBytes));
    if (storage == nullptr) return false;
    HttpBuffer buffer{storage, kPresenceResponseBytes, 0, false};
    cJSON *json = cJSON_CreateObject();
    if (json == nullptr) {
        std::free(storage);
        return false;
    }
    cJSON_AddStringToObject(json, "format", "hub_settings");
    cJSON_AddStringToObject(json, "ingest_path", "hub_self");
    cJSON_AddStringToObject(json, "gateway_guid16", HOME_HUB_GATEWAY_GUID);
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == nullptr) {
        std::free(storage);
        return false;
    }

    esp_http_client_config_t config{};
    config.url = HOME_HUB_INGEST_URL;
    config.event_handler = http_event;
    config.user_data = &buffer;
    config.timeout_ms = 3000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        cJSON_free(body);
        std::free(storage);
        return false;
    }
    char authorization[256]{};
    std::snprintf(authorization, sizeof(authorization), "Bearer %s", HOME_HUB_GATEWAY_TOKEN);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, body, static_cast<int>(std::strlen(body)));
    const esp_err_t result = esp_http_client_perform(client);
    const int status_code = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    const bool success = result == ESP_OK && status_code == 200 && !buffer.overflow &&
                         parse_control_settings(buffer.data, buffer.length);
    esp_http_client_cleanup(client);
    std::memset(authorization, 0, sizeof(authorization));
    cJSON_free(body);
    std::free(storage);
    if (!success) {
        ESP_LOGW(kTag, "Hub settings poll failed: transport=%s http=%d bytes=%u",
                 esp_err_to_name(result), status_code,
                 static_cast<unsigned>(buffer.length));
    }
    return success;
}

bool poll_hub_settings_if_due() {
    const uint32_t now_ms = uptime_ms();
    const uint32_t interval_ms = hub::controlPollIntervalMs(
        g_control_pending.load(), g_control_poll_failures);
    const bool forced = g_force_control_poll.exchange(false);
    if (!forced && g_control_poll_started &&
        static_cast<uint32_t>(now_ms - g_last_control_poll_ms) < interval_ms) {
        return true;
    }

    g_control_poll_started = true;
    g_last_control_poll_ms = now_ms;
    const bool success = fetch_hub_settings();
    if (success) {
        g_control_poll_failures = 0;
    } else if (g_control_poll_failures < 3) {
        ++g_control_poll_failures;
    }

    portENTER_CRITICAL(&g_status_lock);
    g_status.control_poll_seconds = g_control_pending.load()
        ? static_cast<uint8_t>(hub::kControlPollPendingMs / 1000U)
        : static_cast<uint8_t>(hub::kControlPollIdleMs / 1000U);
    portEXIT_CRITICAL(&g_status_lock);
    return success;
}

bool post_hub_presence() {
    const uint32_t now_ms = uptime_ms();
    Status live{};
    portENTER_CRITICAL(&g_status_lock);
    live = g_status;
    portEXIT_CRITICAL(&g_status_lock);
    const bool force_report = g_force_self_report.exchange(false);
    const uint32_t due_ms = g_self_reported
        ? (g_self_report_retry_ms == HOME_HUB_SYNC_INTERVAL_MS
            ? hub::reportingIntervalMs(live.reporting_profile)
            : g_self_report_retry_ms)
        : 0U;
    if (!force_report && g_self_reported &&
        static_cast<uint32_t>(now_ms - g_last_self_report_ms) < due_ms) {
        return true;
    }
    g_last_self_report_ms = now_ms;
    if (!live.wifi_station_connected) return false;

    const bluetooth::Status bluetooth_status = bluetooth::status();
    const testbed::TelemetrySample telemetry = testbed::telemetrySample(now_ms / 1000U);

    cJSON *json = cJSON_CreateObject();
    if (json == nullptr) return false;
    cJSON_AddStringToObject(json, "format", "hub_status");
    cJSON_AddStringToObject(json, "ingest_path", "hub_self");
    cJSON_AddStringToObject(json, "gateway_guid16", HOME_HUB_GATEWAY_GUID);
    cJSON_AddStringToObject(json, "mode", cloud_mode_name(live.effective_mode));
    if (telemetry.has_position) {
        cJSON_AddNumberToObject(json, "latitude", telemetry.latitude);
        cJSON_AddNumberToObject(json, "longitude", telemetry.longitude);
        cJSON_AddNumberToObject(json, "fix_age_s", 0);
    } else {
        cJSON_AddNullToObject(json, "latitude");
        cJSON_AddNullToObject(json, "longitude");
        cJSON_AddNullToObject(json, "fix_age_s");
    }
    if (telemetry.has_battery) {
        cJSON_AddNumberToObject(json, "battery_percent", telemetry.battery_percent);
    } else {
        cJSON_AddNullToObject(json, "battery_percent");
    }
    cJSON_AddBoolToObject(json, "position_simulated", telemetry.position_simulated);
    cJSON_AddBoolToObject(json, "battery_simulated", telemetry.battery_simulated);
    cJSON_AddNumberToObject(json, "uptime_s", now_ms / 1000U);
    cJSON_AddNumberToObject(json, "wifi_rssi_dbm", live.wifi_rssi_dbm);
    cJSON_AddBoolToObject(json, "ble_enabled", live.bluetooth_enabled);
    cJSON_AddBoolToObject(json, "ble_advertising", bluetooth_status.advertising);
    cJSON_AddNumberToObject(json, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(json, "applied_revision",
                            static_cast<double>(live.applied_settings_revision));
    cJSON_AddStringToObject(json, "reporting_profile",
                            hub::reportingProfileName(live.reporting_profile));
    cJSON_AddNumberToObject(json, "report_interval_s",
                            hub::reportingIntervalMs(live.reporting_profile) / 1000U);
    cJSON_AddNumberToObject(json, "control_poll_s", live.control_poll_seconds);

    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == nullptr) return false;
    auto *storage = static_cast<char *>(heap_caps_malloc(
        kPresenceResponseBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (storage == nullptr) storage = static_cast<char *>(std::malloc(kPresenceResponseBytes));
    if (storage == nullptr) {
        cJSON_free(body);
        return false;
    }
    HttpBuffer buffer{storage, kPresenceResponseBytes, 0, false};
    esp_http_client_config_t config{};
    config.url = HOME_HUB_INGEST_URL;
    config.event_handler = http_event;
    config.user_data = &buffer;
    config.timeout_ms = 5000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        std::free(storage);
        cJSON_free(body);
        return false;
    }
    char authorization[256]{};
    std::snprintf(authorization, sizeof(authorization), "Bearer %s", HOME_HUB_GATEWAY_TOKEN);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Authorization", authorization);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, body, static_cast<int>(std::strlen(body)));
    const esp_err_t result = esp_http_client_perform(client);
    const int status_code = result == ESP_OK ? esp_http_client_get_status_code(client) : 0;
    bool accepted = false;
    if (result == ESP_OK && status_code == 200 && !buffer.overflow) {
        cJSON *response = cJSON_ParseWithLength(buffer.data, buffer.length);
        const cJSON *value = response == nullptr ? nullptr
            : cJSON_GetObjectItemCaseSensitive(response, "accepted");
        accepted = cJSON_IsTrue(value);
        cJSON_Delete(response);
        // The self-report response carries the current settings object. Reuse
        // it so a successful heartbeat can avoid a separate settings request.
        if (accepted && parse_control_settings(buffer.data, buffer.length)) {
            g_control_poll_started = true;
            g_last_control_poll_ms = now_ms;
            g_control_poll_failures = 0;
        }
    }
    esp_http_client_cleanup(client);
    std::memset(authorization, 0, sizeof(authorization));
    std::free(storage);
    cJSON_free(body);

    g_self_reported = true;
    g_self_report_retry_ms = accepted ? HOME_HUB_SYNC_INTERVAL_MS
        : std::min(g_self_report_retry_ms * 2U,
                   static_cast<uint32_t>(HOME_HUB_SYNC_MAX_BACKOFF_MS));
    note_self_report_result(accepted, static_cast<uint32_t>(std::max(0, status_code)));
    ESP_LOGI(kTag,
             "Hub self-report: accepted=%d http=%d mode=%s battery=%u%% simulated=%d",
             accepted, status_code, cloud_mode_name(live.effective_mode),
             static_cast<unsigned>(telemetry.battery_percent),
             telemetry.position_simulated || telemetry.battery_simulated);
    return accepted;
}

void reconnect_station(void *) {
    if (!g_station_allowed.load()) return;
    const esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK && error != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(kTag, "Scheduled Wi-Fi reconnect failed to start: %s",
                 esp_err_to_name(error));
    }
}

void schedule_station_reconnect() {
    if (!g_station_allowed.load() || g_reconnect_timer == nullptr) return;
    // A single timer owns retries. Repeated disconnect events can therefore
    // never turn into a hot reconnect loop that starves LVGL or the idle task.
    esp_timer_stop(g_reconnect_timer);
    const esp_err_t error = esp_timer_start_once(g_reconnect_timer,
                                                  kStationReconnectDelayUs);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "Could not schedule Wi-Fi reconnect: %s",
                 esp_err_to_name(error));
    }
}

void wifi_event(void *, esp_event_base_t base, int32_t id, void *) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (g_station_allowed.load()) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi, kConnectedBit);
        xEventGroupSetBits(g_wifi, kDisconnectedBit);
        clear_station_link();
        set_state(ConnectionState::Connecting);
        schedule_station_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (g_reconnect_timer != nullptr) esp_timer_stop(g_reconnect_timer);
        xEventGroupClearBits(g_wifi, kDisconnectedBit);
        xEventGroupSetBits(g_wifi, kConnectedBit);
        refresh_station_link();
        set_state(ConnectionState::Online);
        g_force_control_poll.store(true);
    }
}

hub::Settings network_settings() {
    portENTER_CRITICAL(&g_settings_lock);
    const hub::Settings copy = *g_network_settings;
    portEXIT_CRITICAL(&g_settings_lock);
    return copy;
}

bool configure_wifi(const hub::Settings &settings, unsigned network_index, bool restart,
                    bool off_grid_access_point, bool station_allowed = true) {
    const hub::WifiNetwork &requested = network_index == 1
        ? settings.secondary : settings.primary;
    const hub::WifiNetwork &station = requested;
    const bool station_enabled = station_allowed && hub::validSsid(station.ssid);
    const bool access_point_enabled = off_grid_access_point &&
        hub::validSsid(settings.access_point_ssid);
    if (!station_enabled && !access_point_enabled) return false;

    g_station_allowed.store(false);
    if (g_reconnect_timer != nullptr) esp_timer_stop(g_reconnect_timer);
    clear_station_link();
    if (restart && g_wifi_initialized) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    // The GUI-TION ESP32-C6 firmware acknowledges AP-only mode and raises
    // WIFI_EVENT_AP_START, but does not actually transmit a beacon.  Keep the
    // hosted radio in APSTA whenever the local hotspot is active.  In explicit
    // Off-Grid mode station_enabled remains false and g_station_allowed stays
    // false, so the station interface is idle and makes no uplink attempts.
    const wifi_mode_t mode = access_point_enabled
        ? WIFI_MODE_APSTA
        : WIFI_MODE_STA;
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
    g_station_allowed.store(station_enabled);
    const bool started = esp_wifi_start() == ESP_OK;
    if (started) {
        g_wifi_initialized = true;
        for (const char *key : {"WIFI_AP_DEF", "WIFI_STA_DEF"}) {
            esp_netif_t *network_interface = esp_netif_get_handle_from_ifkey(key);
            if (network_interface != nullptr) {
                esp_netif_set_hostname(network_interface, "bluepaws");
            }
        }
        bluepaws::mdns::start_mdns();
        ESP_LOGI(kTag, "Wi-Fi applied: station=%s off_grid_ap=%s",
                 station_enabled ? station.ssid : "disabled",
                 access_point_enabled ? settings.access_point_ssid : "disabled");
    }
    if (!started) g_station_allowed.store(false);
    return started;
}

bool network_available(const hub::Settings &settings, unsigned index) {
    return hub::validSsid(index == 1 ? settings.secondary.ssid : settings.primary.ssid);
}

unsigned preferred_network(const hub::Settings &settings) {
    return hub::preferredNetwork(settings.communications_mode);
}

bool network_allowed(const hub::Settings &settings, unsigned index) {
    // Portable is an explicit request for the saved secondary/hotspot SSID.
    // It must never silently attach to the home network and present as Home.
    return hub::modeAllowsNetwork(settings.communications_mode, index);
}

bool usable_network(const hub::Settings &settings, unsigned index) {
    return network_allowed(settings, index) && network_available(settings, index);
}

unsigned first_available_network(const hub::Settings &settings, unsigned preferred) {
    if (usable_network(settings, preferred)) return preferred;
    const unsigned alternate = preferred == 0 ? 1U : 0U;
    return usable_network(settings, alternate) ? alternate : preferred;
}

bool start_wifi(const hub::Settings &settings) {
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    if (error != ESP_OK) return false;
    if (esp_netif_init() != ESP_OK || esp_event_loop_create_default() != ESP_OK) return false;
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
    tzset();
    esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    time_config.sync_cb = time_sync_notification;
    if (esp_netif_sntp_init(&time_config) != ESP_OK) {
        ESP_LOGW(kTag, "Could not start NTP time synchronization");
    }
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) return false;
    if (g_reconnect_timer == nullptr) {
        esp_timer_create_args_t reconnect_timer_args{};
        reconnect_timer_args.callback = reconnect_station;
        reconnect_timer_args.name = "wifi_reconnect";
        reconnect_timer_args.skip_unhandled_events = true;
        if (esp_timer_create(&reconnect_timer_args, &g_reconnect_timer) != ESP_OK) {
            ESP_LOGE(kTag, "Could not create Wi-Fi reconnect timer");
            return false;
        }
    }
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr);
    const bool explicit_off_grid =
        settings.communications_mode == hub::CommunicationsMode::OffGrid;
    const unsigned preferred = preferred_network(settings);
    const unsigned network_index = first_available_network(settings, preferred);
    const bool automatic_off_grid = !explicit_off_grid &&
        !usable_network(settings, 0) && !usable_network(settings, 1);
    if (explicit_off_grid || automatic_off_grid) set_off_grid_mode(settings, automatic_off_grid);
    else set_network_mode(settings, network_index);
    return configure_wifi(settings, network_index, false,
                          explicit_off_grid || automatic_off_grid, !explicit_off_grid);
}

void apply_runtime_reconfiguration(hub::Settings &settings,
                                   unsigned &preferred,
                                   unsigned &network_index,
                                   bool &explicit_off_grid,
                                   bool &off_grid_active,
                                   bool &tried_alternate,
                                   uint32_t &cloud_delay_ms) {
    settings = network_settings();
    preferred = preferred_network(settings);
    network_index = first_available_network(settings, preferred);
    explicit_off_grid = settings.communications_mode ==
                        hub::CommunicationsMode::OffGrid;
    off_grid_active = explicit_off_grid ||
        (!usable_network(settings, 0) && !usable_network(settings, 1));
    tried_alternate = network_index != preferred ||
        !usable_network(settings, preferred == 0 ? 1U : 0U);
    xEventGroupClearBits(g_wifi, kConnectedBit | kDisconnectedBit);
    if (off_grid_active) {
        set_state(ConnectionState::Degraded);
        set_off_grid_mode(settings, !explicit_off_grid);
    } else {
        set_state(ConnectionState::Connecting);
        set_network_mode(settings, network_index);
    }
    if (!configure_wifi(settings, network_index, true, off_grid_active,
                        !explicit_off_grid)) {
        set_state(ConnectionState::Degraded);
        ESP_LOGE(kTag, "Wi-Fi reconfiguration failed; networking left degraded");
    }
    cloud_delay_ms = HOME_HUB_SYNC_INTERVAL_MS;
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
    uint32_t cloud_delay_ms = HOME_HUB_SYNC_INTERVAL_MS;
    unsigned preferred = preferred_network(settings);
    unsigned network_index = first_available_network(settings, preferred);
    bool explicit_off_grid = settings.communications_mode == hub::CommunicationsMode::OffGrid;
    bool off_grid_active = explicit_off_grid ||
        (!usable_network(settings, 0) && !usable_network(settings, 1));
    bool tried_alternate = network_index != preferred ||
        !usable_network(settings, preferred == 0 ? 1U : 0U);
    if (off_grid_active) set_state(ConnectionState::Degraded);
    while (true) {
        xEventGroupClearBits(g_wifi, kDisconnectedBit);
        const bool any_network_configured = usable_network(settings, 0) ||
                                            usable_network(settings, 1);
        if (explicit_off_grid || (off_grid_active && !any_network_configured)) {
            set_state(ConnectionState::Degraded);
            set_off_grid_mode(settings, !explicit_off_grid);
            // AP-only states have no station timeout. A user selection or newly
            // saved credential wakes this task immediately through reconfigure.
            const EventBits_t events = xEventGroupWaitBits(
                g_wifi, kReconfigureBit | kWifiScanBit,
                pdTRUE, pdFALSE, portMAX_DELAY);
            const bool scanned = handle_wifi_scan(events);
            if ((events & kReconfigureBit) != 0) {
                apply_runtime_reconfiguration(settings, preferred, network_index,
                                              explicit_off_grid, off_grid_active,
                                              tried_alternate, cloud_delay_ms);
                continue;
            }
            if (scanned) continue;
            // Defensive yield: an unexpected wake-up must never become a
            // priority-5 busy loop, even if a future event bit is added here.
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        const bool both_configured = usable_network(settings, 0) &&
                                     usable_network(settings, 1);
        const uint32_t recovery_wait_ms = off_grid_active
            ? HOME_HUB_WIFI_RECOVERY_MS
            : (both_configured ? HOME_HUB_WIFI_RECOVERY_MS / 2U
                               : HOME_HUB_WIFI_RECOVERY_MS);
        const EventBits_t connected = xEventGroupWaitBits(
            g_wifi,
            kConnectedBit | kReconfigureBit | kWifiScanBit,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(recovery_wait_ms));
        if (handle_wifi_scan(connected)) continue;
        if ((connected & kReconfigureBit) != 0) {
            xEventGroupClearBits(g_wifi, kReconfigureBit | kConnectedBit);
            apply_runtime_reconfiguration(settings, preferred, network_index,
                                          explicit_off_grid, off_grid_active,
                                          tried_alternate, cloud_delay_ms);
            continue;
        }
        if ((connected & kConnectedBit) != 0) {
            refresh_station_link();
        }
        if ((connected & kConnectedBit) == 0) {
            const bool primary_available = usable_network(settings, 0);
            const bool secondary_available = usable_network(settings, 1);
            if (!primary_available && !secondary_available) {
                off_grid_active = true;
                set_state(ConnectionState::Degraded);
                set_off_grid_mode(settings, true);
                continue;
            }

            const unsigned alternate = network_index == 0 ? 1U : 0U;
            if (!off_grid_active && !tried_alternate && usable_network(settings, alternate)) {
                network_index = alternate;
                tried_alternate = true;
                set_state(ConnectionState::Connecting);
                set_network_mode(settings, network_index);
                ESP_LOGI(kTag, "Preferred Wi-Fi timed out; trying %s uplink",
                         network_index == 1 ? "secondary" : "primary");
                configure_wifi(settings, network_index, true, false);
                continue;
            }

            if (!off_grid_active) {
                off_grid_active = true;
                set_state(ConnectionState::Degraded);
                set_off_grid_mode(settings, true);
                ESP_LOGW(kTag, "Known Wi-Fi recovery timed out; automatic off-grid AP enabled");
            } else if (usable_network(settings, alternate)) {
                network_index = alternate;
            }
            configure_wifi(settings, network_index, true, true);
            continue;
        } else if (off_grid_active) {
            // An automatic fallback closes only after a known uplink returns.
            // Explicit Off-Grid never reaches this branch.
            if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
                off_grid_active = false;
                tried_alternate = network_index != preferred;
                set_network_mode(settings, network_index);
                ESP_LOGI(kTag, "Trusted Wi-Fi restored; automatic off-grid AP stopped");
            }
            cloud_delay_ms = HOME_HUB_SYNC_INTERVAL_MS;
        } else {
            if (g_cloud_authorized) post_hub_presence();
            if (g_cloud_authorized) poll_hub_settings_if_due();
            const bool snapshot_ok = !g_cloud_authorized || fetch_snapshot();
            if (snapshot_ok) {
                set_network_mode(settings, network_index);
                cloud_delay_ms = HOME_HUB_SYNC_INTERVAL_MS;
            } else {
                cloud_delay_ms = std::min(cloud_delay_ms * 2U,
                                          static_cast<uint32_t>(HOME_HUB_SYNC_MAX_BACKOFF_MS));
            }
        }
        // A manual mode selection or link loss interrupts cloud backoff immediately.
        const EventBits_t events = xEventGroupWaitBits(
            g_wifi, kReconfigureBit | kDisconnectedBit | kWifiScanBit,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(cloud_delay_ms));
        handle_wifi_scan(events);
    }
}

}  // namespace

bool start(const hub::Settings &settings) {
    g_cloud_authorized = std::strlen(HOME_HUB_GATEWAY_TOKEN) >= 16;
    if (!g_cloud_authorized) {
        ESP_LOGW(kTag, "Cloud sync disabled: home_hub_secrets.h is not provisioned");
        set_state(ConnectionState::Disabled);
    }
    if (g_network_settings == nullptr) {
        g_network_settings = static_cast<hub::Settings *>(heap_caps_malloc(
            sizeof(hub::Settings), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (g_network_settings == nullptr) return false;
    portENTER_CRITICAL(&g_settings_lock);
    *g_network_settings = settings;
    portEXIT_CRITICAL(&g_settings_lock);
    portENTER_CRITICAL(&g_status_lock);
    g_status.applied_settings_revision = settings.cloud_settings_revision;
    g_status.bluetooth_enabled = settings.bluetooth_enabled;
    g_status.reporting_profile = settings.reporting_profile;
    g_status.control_poll_seconds = hub::kControlPollIdleMs / 1000U;
    portEXIT_CRITICAL(&g_status_lock);
    g_updates = xQueueCreate(kMaximumCats * 2, sizeof(CloudUpdate));
    g_controls = xQueueCreate(1, sizeof(ControlUpdate));
    g_wifi = xEventGroupCreate();
    if (g_wifi_scan == nullptr) {
        g_wifi_scan = static_cast<WifiScanSnapshot *>(
            heap_caps_calloc(1, sizeof(WifiScanSnapshot),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (g_updates == nullptr || g_controls == nullptr || g_wifi == nullptr ||
        g_wifi_scan == nullptr) return false;
    if (g_cloud_authorized) restore_cached_snapshot();
    const bool started = xTaskCreate(sync_task, "hub_cloud", 12288, nullptr, 5, nullptr) == pdPASS;
    return started && g_cloud_authorized;
}

bool applyNetworkSettings(const hub::Settings &input) {
    if (g_network_settings == nullptr) return false;
    hub::Settings settings = input;
    hub::sanitize(settings);
    portENTER_CRITICAL(&g_settings_lock);
    *g_network_settings = settings;
    portEXIT_CRITICAL(&g_settings_lock);
    if (g_wifi != nullptr) xEventGroupSetBits(g_wifi, kReconfigureBit);
    return true;
}

bool requestWifiScan() {
    if (g_wifi == nullptr || g_wifi_scan == nullptr || !g_wifi_initialized) return false;
    portENTER_CRITICAL(&g_wifi_scan_lock);
    if (g_wifi_scan->state == WifiScanState::Scanning) {
        portEXIT_CRITICAL(&g_wifi_scan_lock);
        return true;
    }
    g_wifi_scan->state = WifiScanState::Scanning;
    g_wifi_scan->count = 0;
    ++g_wifi_scan->generation;
    portEXIT_CRITICAL(&g_wifi_scan_lock);
    xEventGroupSetBits(g_wifi, kWifiScanBit);
    return true;
}

WifiScanSnapshot wifiScanSnapshot() {
    if (g_wifi_scan == nullptr) return {};
    portENTER_CRITICAL(&g_wifi_scan_lock);
    const WifiScanSnapshot snapshot = *g_wifi_scan;
    portEXIT_CRITICAL(&g_wifi_scan_lock);
    return snapshot;
}

const char *modeReasonName(ModeReason reason) {
    switch (reason) {
    case ModeReason::PrimaryWifi: return "primary_wifi";
    case ModeReason::SecondaryWifi: return "secondary_wifi";
    case ModeReason::WifiUnavailable: return "wifi_unavailable";
    case ModeReason::ManualSelection: return "manual_selection";
    }
    return "unknown";
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

bool takeControlUpdate(ControlUpdate &update) {
    return g_controls != nullptr && xQueueReceive(g_controls, &update, 0) == pdTRUE;
}

void acknowledgeControlUpdate(uint64_t revision, bool bluetooth_enabled,
                              hub::ReportingProfile reporting_profile) {
    portENTER_CRITICAL(&g_status_lock);
    if (revision >= g_status.applied_settings_revision) {
        g_status.applied_settings_revision = revision;
        g_status.bluetooth_enabled = bluetooth_enabled;
        g_status.reporting_profile = reporting_profile;
        g_status.control_poll_seconds = hub::kControlPollIdleMs / 1000U;
    }
    portEXIT_CRITICAL(&g_status_lock);
    g_control_pending.store(false);
    g_force_self_report.store(true);
    ESP_LOGI(kTag, "Hub settings revision %llu applied: Bluetooth=%s profile=%s",
             static_cast<unsigned long long>(revision),
             bluetooth_enabled ? "On" : "Off",
             hub::reportingProfileName(reporting_profile));
}

Status status() {
    portENTER_CRITICAL(&g_status_lock);
    const Status copy = g_status;
    portEXIT_CRITICAL(&g_status_lock);
    return copy;
}

}  // namespace bluepaws::cloud
