#include "home_hub_settings_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <algorithm>
#include <cstring>

namespace bluepaws::settings_store {
namespace {

constexpr char kTag[] = "hub_settings";
constexpr char kNamespace[] = "bp_hub";

bool initialize_nvs() {
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK) return false;
        error = nvs_flash_init();
    }
    return error == ESP_OK;
}

void get_string(nvs_handle_t handle, const char *key, char *destination, std::size_t capacity) {
    std::size_t required = capacity;
    if (nvs_get_str(handle, key, destination, &required) != ESP_OK) return;
    destination[capacity - 1] = '\0';
}

uint16_t get_u16(nvs_handle_t handle, const char *key, uint16_t fallback) {
    uint16_t value = fallback;
    return nvs_get_u16(handle, key, &value) == ESP_OK ? value : fallback;
}

uint8_t get_u8(nvs_handle_t handle, const char *key, uint8_t fallback) {
    uint8_t value = fallback;
    return nvs_get_u8(handle, key, &value) == ESP_OK ? value : fallback;
}

bool set_string(nvs_handle_t handle, const char *key, const char *value) {
    return nvs_set_str(handle, key, value) == ESP_OK;
}

}  // namespace

bool load(hub::Settings &settings) {
    if (!initialize_nvs()) {
        ESP_LOGE(kTag, "NVS initialization failed; using firmware defaults");
        hub::sanitize(settings);
        return false;
    }
    nvs_handle_t handle{};
    if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        hub::sanitize(settings);
        return false;
    }
    get_string(handle, "pri_ssid", settings.primary.ssid, sizeof(settings.primary.ssid));
    get_string(handle, "pri_pass", settings.primary.password, sizeof(settings.primary.password));
    get_string(handle, "sec_ssid", settings.secondary.ssid, sizeof(settings.secondary.ssid));
    get_string(handle, "sec_pass", settings.secondary.password, sizeof(settings.secondary.password));
    get_string(handle, "ap_ssid", settings.access_point_ssid, sizeof(settings.access_point_ssid));
    get_string(handle, "ap_pass", settings.access_point_password, sizeof(settings.access_point_password));
    settings.overview_timeout_seconds = get_u16(handle, "overview_s", settings.overview_timeout_seconds);
    settings.dim_timeout_seconds = get_u16(handle, "dim_s", settings.dim_timeout_seconds);
    settings.screen_off_timeout_seconds = get_u16(handle, "off_s", settings.screen_off_timeout_seconds);
    settings.dim_brightness_percent = get_u8(handle, "dim_pct", settings.dim_brightness_percent);
    settings.brightness_percent = get_u8(handle, "bright_pct", settings.brightness_percent);
    settings.volume_percent = get_u8(handle, "volume_pct", settings.volume_percent);
    nvs_close(handle);
    hub::sanitize(settings);
    ESP_LOGI(kTag, "Loaded Home Hub settings from NVS");
    return true;
}

bool save(const hub::Settings &input) {
    if (!initialize_nvs()) return false;
    hub::Settings settings = input;
    hub::sanitize(settings);
    nvs_handle_t handle{};
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    bool ok = true;
    ok = set_string(handle, "pri_ssid", settings.primary.ssid) && ok;
    ok = set_string(handle, "pri_pass", settings.primary.password) && ok;
    ok = set_string(handle, "sec_ssid", settings.secondary.ssid) && ok;
    ok = set_string(handle, "sec_pass", settings.secondary.password) && ok;
    ok = set_string(handle, "ap_ssid", settings.access_point_ssid) && ok;
    ok = set_string(handle, "ap_pass", settings.access_point_password) && ok;
    ok = nvs_set_u16(handle, "overview_s", settings.overview_timeout_seconds) == ESP_OK && ok;
    ok = nvs_set_u16(handle, "dim_s", settings.dim_timeout_seconds) == ESP_OK && ok;
    ok = nvs_set_u16(handle, "off_s", settings.screen_off_timeout_seconds) == ESP_OK && ok;
    ok = nvs_set_u8(handle, "dim_pct", settings.dim_brightness_percent) == ESP_OK && ok;
    ok = nvs_set_u8(handle, "bright_pct", settings.brightness_percent) == ESP_OK && ok;
    ok = nvs_set_u8(handle, "volume_pct", settings.volume_percent) == ESP_OK && ok;
    ok = nvs_commit(handle) == ESP_OK && ok;
    nvs_close(handle);
    if (ok) ESP_LOGI(kTag, "Saved Home Hub settings to NVS");
    else ESP_LOGE(kTag, "Could not persist Home Hub settings");
    return ok;
}

}  // namespace bluepaws::settings_store
