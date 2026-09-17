#ifndef BLUEPAWS_HOME_HUB_CLOUD_H
#define BLUEPAWS_HOME_HUB_CLOUD_H

#include "bluepaws/cat_store.h"
#include "bluepaws/hub_settings.h"

#include <cstddef>
#include <cstdint>
#include <array>

namespace bluepaws::cloud {

enum class ConnectionState : uint8_t {
    Disabled,
    Starting,
    Connecting,
    Online,
    Degraded,
};

// Explains why the runtime mode differs from (or matches) the user's saved
// policy.  The requested mode is persistent; the effective mode follows the
// radio path that is actually active.
enum class ModeReason : uint8_t {
    ManualSelection,
    PrimaryWifi,
    SecondaryWifi,
    WifiUnavailable,
};

struct Status {
    ConnectionState state = ConnectionState::Disabled;
    uint32_t successful_snapshots = 0;
    uint32_t failed_snapshots = 0;
    uint32_t last_http_status = 0;
    uint32_t last_sync_uptime_ms = 0;
    hub::CommunicationsMode requested_mode = hub::CommunicationsMode::Home;
    hub::CommunicationsMode effective_mode = hub::CommunicationsMode::Home;
    ModeReason mode_reason = ModeReason::ManualSelection;
    bool automatic_off_grid = false;
    bool time_synchronized = false;
    bool wifi_station_connected = false;
    int16_t wifi_rssi_dbm = -127;
    char wifi_ssid[33]{};
};

constexpr std::size_t kMaximumWifiScanResults = 12;

enum class WifiScanState : uint8_t {
    Idle,
    Scanning,
    Ready,
    Failed,
};

struct WifiScanResult {
    char ssid[33]{};
    int8_t rssi_dbm = -127;
    bool secured = false;
};

struct WifiScanSnapshot {
    WifiScanState state = WifiScanState::Idle;
    uint32_t generation = 0;
    std::size_t count = 0;
    std::array<WifiScanResult, kMaximumWifiScanResults> results{};
};

// Starts ESP-Hosted Wi-Fi and the HTTPS snapshot task. Returns false when the
// local gateway credential is not configured or task creation fails.
bool start(const hub::Settings &settings);

// Applies saved primary/secondary station credentials and automatic fallback
// AP settings on the networking task. The UI never calls esp_wifi directly.
bool applyNetworkSettings(const hub::Settings &settings);

// Queues a non-blocking nearby-network scan on the networking task. In
// Off-Grid mode the AP remains active while a temporary STA interface scans.
bool requestWifiScan();
WifiScanSnapshot wifiScanSnapshot();

// Called only by the LVGL/main task. Cloud work never mutates UI state from
// its networking task, avoiding cross-thread LVGL and CatStore access.
std::size_t drain(CatStore &store);
Status status();
const char *modeReasonName(ModeReason reason);

}  // namespace bluepaws::cloud

#endif
