#ifndef BLUEPAWS_HOME_HUB_CLOUD_H
#define BLUEPAWS_HOME_HUB_CLOUD_H

#include "bluepaws/cat_store.h"
#include "bluepaws/hub_settings.h"

#include <cstddef>
#include <cstdint>

namespace bluepaws::cloud {

enum class ConnectionState : uint8_t {
    Disabled,
    Starting,
    Connecting,
    Online,
    Degraded,
};

struct Status {
    ConnectionState state = ConnectionState::Disabled;
    uint32_t successful_snapshots = 0;
    uint32_t failed_snapshots = 0;
    uint32_t last_http_status = 0;
    uint32_t last_sync_uptime_ms = 0;
};

// Starts ESP-Hosted Wi-Fi and the HTTPS snapshot task. Returns false when the
// local gateway credential is not configured or task creation fails.
bool start(const hub::Settings &settings);

// Applies saved primary/secondary station credentials and automatic fallback
// AP settings on the networking task. The UI never calls esp_wifi directly.
bool applyNetworkSettings(const hub::Settings &settings);

// Called only by the LVGL/main task. Cloud work never mutates UI state from
// its networking task, avoiding cross-thread LVGL and CatStore access.
std::size_t drain(CatStore &store);
Status status();

}  // namespace bluepaws::cloud

#endif
