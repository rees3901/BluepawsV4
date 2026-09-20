#include "home_hub_bluetooth.h"

extern "C" {
#include "esp_hosted_misc.h"
}
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace bluepaws::bluetooth {
namespace {

constexpr char kTag[] = "home_hub_ble";
constexpr char kBeaconName[] = "BLUEPAWS_HOME";
constexpr char kFindBeaconPrefix[] = "BP_FIND_";
constexpr std::size_t kMaximumScanResults = 16;

struct StoredScanResult {
    uint16_t device_id = 0;
    int8_t rssi = -127;
    int64_t seen_at_us = 0;
};

std::atomic_bool g_worker_started{false};
std::atomic_bool g_initialized{false};
std::atomic_bool g_synced{false};
std::atomic_bool g_advertising{false};
std::atomic_bool g_scanning{false};
std::atomic_bool g_desired_enabled{false};
std::atomic_int g_desired_mode{static_cast<int>(hub::CommunicationsMode::Home)};
std::atomic<int64_t> g_scan_deadline_us{0};
std::array<StoredScanResult, kMaximumScanResults> g_scan_results{};
std::size_t g_scan_result_count = 0;
portMUX_TYPE g_scan_results_lock = portMUX_INITIALIZER_UNLOCKED;
uint8_t g_own_address_type = 0;

bool should_advertise()
{
    // Home advertising is a fail-closed policy: Portable and Off-Grid always
    // override the saved preference so collars cannot be told they are home.
    return g_desired_enabled.load() &&
           g_desired_mode.load() == static_cast<int>(hub::CommunicationsMode::Home);
}

bool should_scan()
{
    return g_desired_mode.load() != static_cast<int>(hub::CommunicationsMode::Home) &&
           esp_timer_get_time() < g_scan_deadline_us.load();
}

void store_scan_result(const ble_gap_disc_desc &discovery)
{
    ble_hs_adv_fields fields{};
    if (ble_hs_adv_parse_fields(&fields, discovery.data, discovery.length_data) != 0 ||
        fields.name == nullptr ||
        fields.name_len < sizeof(kFindBeaconPrefix) - 1U + 4U ||
        std::memcmp(fields.name, kFindBeaconPrefix, sizeof(kFindBeaconPrefix) - 1U) != 0) {
        return;
    }
    char identifier[5]{};
    std::memcpy(identifier, fields.name + sizeof(kFindBeaconPrefix) - 1U, 4U);
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(identifier, &end, 16);
    if (end != identifier + 4 || parsed == 0 || parsed > UINT16_MAX) return;

    portENTER_CRITICAL(&g_scan_results_lock);
    StoredScanResult *slot = nullptr;
    for (std::size_t index = 0; index < g_scan_result_count; ++index) {
        if (g_scan_results[index].device_id == parsed) {
            slot = &g_scan_results[index];
            break;
        }
    }
    if (slot == nullptr && g_scan_result_count < g_scan_results.size()) {
        slot = &g_scan_results[g_scan_result_count++];
        slot->device_id = static_cast<uint16_t>(parsed);
    }
    if (slot != nullptr) {
        slot->rssi = discovery.rssi;
        slot->seen_at_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&g_scan_results_lock);
}

int gap_event(ble_gap_event *event, void *)
{
    if (event == nullptr) return 0;
    if (event->type == BLE_GAP_EVENT_DISC) {
        store_scan_result(event->disc);
    } else if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        g_advertising = false;
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        g_scanning = false;
    }
    return 0;
}

bool begin_advertising()
{
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    const char *name = ble_svc_gap_device_name();
    fields.name = reinterpret_cast<uint8_t *>(const_cast<char *>(name));
    fields.name_len = std::strlen(name);
    fields.name_is_complete = 1;
    int result = ble_gap_adv_set_fields(&fields);
    if (result != 0) {
        ESP_LOGE(kTag, "Could not configure Home beacon: %d", result);
        return false;
    }

    ble_gap_adv_params parameters{};
    parameters.conn_mode = BLE_GAP_CONN_MODE_NON;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    result = ble_gap_adv_start(g_own_address_type, nullptr, BLE_HS_FOREVER,
                               &parameters, gap_event, nullptr);
    if (result != 0) {
        ESP_LOGE(kTag, "Could not start Home beacon: %d", result);
        return false;
    }
    g_advertising = true;
    ESP_LOGI(kTag, "%s is advertising", kBeaconName);
    return true;
}

void stop_advertising()
{
    if (!g_advertising.load()) return;
    const int result = ble_gap_adv_stop();
    if (result != 0) {
        ESP_LOGW(kTag, "Could not stop Home beacon: %d", result);
        return;
    }
    g_advertising = false;
    ESP_LOGI(kTag, "Home beacon stopped");
}

bool begin_passive_scan()
{
    ble_gap_disc_params parameters{};
    parameters.passive = 1;
    parameters.filter_duplicates = 1;
    // NimBLE's zero/default values select a 30 ms window every 30 ms: a
    // continuous 100% BLE scan.  The C6 shares one 2.4 GHz radio with the
    // Off-Grid SoftAP, so that can starve Wi-Fi beacons and associations.
    // A 10% listening duty cycle is used inside a user-requested five-second
    // scan. There is no background scan in Portable or Off-Grid mode.
    parameters.itvl = BLE_GAP_SCAN_ITVL_MS(300);
    parameters.window = BLE_GAP_SCAN_WIN_MS(30);
    const int result = ble_gap_disc(g_own_address_type, BLE_HS_FOREVER,
                                    &parameters, gap_event, nullptr);
    if (result != 0) {
        ESP_LOGE(kTag, "Could not start passive collar scan: %d", result);
        return false;
    }
    g_scanning = true;
    ESP_LOGI(kTag, "Passive BLE listening active; Home advertising disabled");
    return true;
}

void stop_passive_scan()
{
    if (!g_scanning.load()) return;
    const int result = ble_gap_disc_cancel();
    if (result != 0) {
        ESP_LOGW(kTag, "Could not stop passive collar scan: %d", result);
        return;
    }
    g_scanning = false;
    ESP_LOGI(kTag, "Passive BLE listening stopped");
}

void on_reset(int reason)
{
    g_synced = false;
    g_advertising = false;
    g_scanning = false;
    g_scan_deadline_us = 0;
    ESP_LOGW(kTag, "NimBLE reset: %d", reason);
}

void on_sync()
{
    int result = ble_hs_util_ensure_addr(0);
    if (result == 0) result = ble_hs_id_infer_auto(0, &g_own_address_type);
    if (result != 0) {
        ESP_LOGE(kTag, "Could not determine Bluetooth address: %d", result);
        return;
    }
    g_synced = true;
    ESP_LOGI(kTag, "NimBLE synchronized with ESP32-C6 controller");
}

void host_task(void *)
{
    ESP_LOGI(kTag, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool initialize_stack()
{
    const int64_t request_started_us = esp_timer_get_time();
    const esp_err_t controller_init = esp_hosted_bt_controller_init();
    const int64_t request_elapsed_ms =
        (esp_timer_get_time() - request_started_us) / 1000;
    if (controller_init != ESP_OK && request_elapsed_ms < 500) {
        // The shared SDIO transport is still coming up. Do not start NimBLE
        // yet: doing so would initialize ESP-Hosted a second time and race the
        // Wi-Fi worker for the same bus.
        return false;
    }
    if (controller_init == ESP_OK) {
        const esp_err_t controller_enable = esp_hosted_bt_controller_enable();
        if (controller_enable != ESP_OK) {
            ESP_LOGW(kTag,
                     "ESP32-C6 controller enable RPC failed (%s); trying legacy HCI mode",
                     esp_err_to_name(controller_enable));
        }
    } else {
        // Early GUI-TION C6 firmware starts its BLE controller during boot and
        // exposes HCI over SDIO, but predates ESP-Hosted's FeatureControl RPC.
        // The advertised HCI transport is therefore already usable even though
        // the newer controller-init request returns unsupported/times out.
        ESP_LOGW(kTag,
                 "ESP32-C6 controller-init RPC unavailable (%s); using legacy HCI mode",
                 esp_err_to_name(controller_init));
    }
    const esp_err_t nimble_init = nimble_port_init();
    if (nimble_init != ESP_OK) {
        ESP_LOGE(kTag, "NimBLE initialization failed: %s", esp_err_to_name(nimble_init));
        return false;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    const int name_result = ble_svc_gap_device_name_set(kBeaconName);
    if (name_result != 0) {
        ESP_LOGE(kTag, "Could not set Bluetooth name: %d", name_result);
        return false;
    }
    nimble_port_freertos_init(host_task);
    g_initialized = true;
    ESP_LOGI(kTag, "Bluetooth host initialized");
    return true;
}

void control_task(void *)
{
    // Wi-Fi initializes the shared ESP-Hosted transport asynchronously. Retry
    // quietly until that link is active instead of delaying app startup.
    while (!initialize_stack()) vTaskDelay(pdMS_TO_TICKS(1000));
    while (true) {
        if (g_synced.load()) {
            const bool advertise = should_advertise();
            const bool scan = should_scan();

            // Stop the disallowed role first. In particular, Off-Grid and
            // Portable never permit even a brief overlap with the Home beacon.
            if (!advertise && g_advertising.load()) stop_advertising();
            if (!scan && g_scanning.load()) stop_passive_scan();

            if (advertise && !g_advertising.load() && !g_scanning.load()) {
                begin_advertising();
            } else if (scan && !g_scanning.load() && !g_advertising.load()) {
                begin_passive_scan();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

}  // namespace

bool start(bool enabled, hub::CommunicationsMode mode)
{
    apply(enabled, mode);
    bool expected = false;
    if (!g_worker_started.compare_exchange_strong(expected, true)) return true;
    if (xTaskCreate(control_task, "hub_bluetooth", 6144, nullptr, 4, nullptr) != pdPASS) {
        g_worker_started = false;
        ESP_LOGE(kTag, "Could not create Bluetooth control task");
        return false;
    }
    return true;
}

void apply(bool enabled, hub::CommunicationsMode mode)
{
    g_desired_enabled = enabled;
    g_desired_mode = static_cast<int>(mode);
    if (mode == hub::CommunicationsMode::Home) g_scan_deadline_us = 0;
}

bool requestScan(uint32_t duration_ms)
{
    if (!g_initialized.load() || !g_synced.load() ||
        g_desired_mode.load() == static_cast<int>(hub::CommunicationsMode::Home) ||
        duration_ms == 0) return false;
    portENTER_CRITICAL(&g_scan_results_lock);
    g_scan_result_count = 0;
    g_scan_results = {};
    portEXIT_CRITICAL(&g_scan_results_lock);
    g_scan_deadline_us = esp_timer_get_time() + static_cast<int64_t>(duration_ms) * 1000;
    ESP_LOGI(kTag, "User-requested passive BLE scan queued for %lu ms",
             static_cast<unsigned long>(duration_ms));
    return true;
}

std::size_t scanResults(ScanResult *results, std::size_t capacity)
{
    if (results == nullptr || capacity == 0) return 0;
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&g_scan_results_lock);
    const std::size_t count = std::min(capacity, g_scan_result_count);
    for (std::size_t index = 0; index < count; ++index) {
        results[index] = {
            .device_id = g_scan_results[index].device_id,
            .rssi = g_scan_results[index].rssi,
            .age_ms = static_cast<uint32_t>(
                std::max<int64_t>(0, now_us - g_scan_results[index].seen_at_us) / 1000),
        };
    }
    portEXIT_CRITICAL(&g_scan_results_lock);
    return count;
}

Status status()
{
    const bool desired_advertising = should_advertise();
    const bool desired_scanning = should_scan();
    const bool ready = g_initialized.load() && g_synced.load();
    return {
        .initialized = ready,
        .enabled = g_advertising.load() || g_scanning.load(),
        .advertising = g_advertising.load(),
        .scanning = g_scanning.load(),
        .settled = ready &&
                   g_advertising.load() == desired_advertising &&
                   g_scanning.load() == desired_scanning,
    };
}

}  // namespace bluepaws::bluetooth
