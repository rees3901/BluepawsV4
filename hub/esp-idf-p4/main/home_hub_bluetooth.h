#pragma once

#include "bluepaws/hub_settings.h"

namespace bluepaws::bluetooth {

struct Status {
    bool initialized = false;
    bool enabled = false;
    bool advertising = false;
    bool scanning = false;
    bool settled = false;
};

// Starts the non-blocking Bluetooth worker. The ESP32-P4 runs the NimBLE host
// while the board's ESP32-C6 supplies the controller over ESP-Hosted VHCI.
bool start(bool enabled, hub::CommunicationsMode mode);

// Updates the persisted user preference and current mode presented to the
// worker. Bluetooth advertises only while both are Home/On. Portable and
// Off-Grid are hard overrides that stop advertising and use passive scanning.
void apply(bool enabled, hub::CommunicationsMode mode);

Status status();

}  // namespace bluepaws::bluetooth
