#pragma once

#include "bluepaws/cat_store.h"
#include "home_hub_cloud.h"

namespace bluepaws::web {

// Mounts the bundled public dashboard and starts the local HTTP service. The
// server binds once and remains available on either the station LAN or the
// hub's Off-Grid access point as network roles change.
bool start();

// Publishes an immutable copy for the web task. Call only from the LVGL/main
// task after applying cloud or local telemetry updates.
void updateSnapshot(const CatStore &cats, const cloud::Status &cloud_status);

}  // namespace bluepaws::web
