#pragma once

#include "bluepaws/cat_store.h"
#include "home_hub_bluetooth.h"
#include "home_hub_cloud.h"

namespace bluepaws::web {

// Mounts the bundled public dashboard and starts the local HTTP service. The
// server binds once and remains available on either the station LAN or the
// hub's Off-Grid access point as network roles change.
bool start();

// Transfers a mode request from the HTTP task to the LVGL/main task. Network
// settings and UI state are applied together there, never from the web task.
bool takeRequestedMode(hub::CommunicationsMode &mode);

// Transfers a Bluetooth preference request from HTTP to the main task so NVS,
// the web snapshot and the radio are changed as one operation.
bool takeRequestedBluetooth(bool &enabled);

// Publishes an immutable copy for the web task. Call only from the LVGL/main
// task after applying cloud or local telemetry updates.
void updateSnapshot(const CatStore &cats, const cloud::Status &cloud_status,
                    const hub::Settings &settings,
                    const bluetooth::Status &bluetooth_status);

}  // namespace bluepaws::web
