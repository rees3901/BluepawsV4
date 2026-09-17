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
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include <atomic>
#include <cstring>

namespace bluepaws::bluetooth {
namespace {

constexpr char kTag[] = "home_hub_ble";
constexpr char kBeaconName[] = "BLUEPAWS_HOME";

std::atomic_bool g_worker_started{false};
std::atomic_bool g_initialized{false};
std::atomic_bool g_synced{false};
std::atomic_bool g_advertising{false};
std::atomic_bool g_scanning{false};
std::atomic_bool g_desired_enabled{false};
std::atomic_int g_desired_mode{static_cast<int>(hub::CommunicationsMode::Home)};
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
    return g_desired_enabled.load() &&
           g_desired_mode.load() != static_cast<int>(hub::CommunicationsMode::Home);
}

int gap_event(ble_gap_event *event, void *)
{
    if (event == nullptr) return 0;
    if (event->type == BLE_GAP_EVENT_ADV_COMPLETE) {
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
    // A 10% listening duty cycle still detects collar advertisements quickly
    // while leaving deterministic airtime for the local hotspot.
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
        vTaskDelay(pdMS_TO_TICKS(250));
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
}

Status status()
{
    const bool desired_advertising = should_advertise();
    const bool desired_scanning = should_scan();
    const bool ready = g_initialized.load() && g_synced.load();
    return {
        .initialized = ready,
        .enabled = g_desired_enabled.load(),
        .advertising = g_advertising.load(),
        .scanning = g_scanning.load(),
        .settled = ready &&
                   g_advertising.load() == desired_advertising &&
                   g_scanning.load() == desired_scanning,
    };
}

}  // namespace bluepaws::bluetooth
