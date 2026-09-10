#pragma once

#include "bluepaws/hub_settings.h"

namespace bluepaws::settings_store {

// Loads any saved values over the supplied defaults. Invalid or obsolete
// values are sanitized before they reach either the UI or Wi-Fi adapter.
bool load(hub::Settings &settings);

// Persists settings in the P4's NVS. Passwords never go to the SD card or logs.
bool save(const hub::Settings &settings);

}  // namespace bluepaws::settings_store
