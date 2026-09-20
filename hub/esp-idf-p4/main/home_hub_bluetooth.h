#pragma once

#include "bluepaws/hub_settings.h"

#include <cstddef>
#include <cstdint>

namespace bluepaws::bluetooth {

struct Status {
    bool initialized = false;
    bool enabled = false;
    bool advertising = false;
    bool scanning = false;
    bool settled = false;
};

struct ScanResult {
    uint16_t device_id = 0;
    int8_t rssi = -127;
    uint32_t age_ms = 0;
};

// Starts the non-blocking Bluetooth worker. The ESP32-P4 runs the NimBLE host
// while the board's ESP32-C6 supplies the controller over ESP-Hosted VHCI.
bool start(bool enabled, hub::CommunicationsMode mode);

// Updates the persisted user preference and current mode presented to the
// worker. Bluetooth advertises only while both are Home/On. Portable and
// Off-Grid are hard overrides that leave the radio idle until requestScan().
void apply(bool enabled, hub::CommunicationsMode mode);

// Starts one bounded, user-requested passive collar scan. Home mode rejects
// the request so the Home beacon role is never mixed with collar discovery.
bool requestScan(uint32_t duration_ms = 5000);

// Copies the most recent BP_FIND_XXXX results into the caller's buffer.
std::size_t scanResults(ScanResult *results, std::size_t capacity);

Status status();

}  // namespace bluepaws::bluetooth
