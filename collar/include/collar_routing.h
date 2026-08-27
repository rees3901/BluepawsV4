#pragma once

#include <bp_protocol.h>

// Provision this explicitly for each collar. It is an identity, not an IP
// address, and must not change when Wi-Fi/BLE connectivity changes.
#ifndef MY_HOME_HUB_ID
#error "Provision MY_HOME_HUB_ID in collar_secrets.h or a build flag"
#endif
static_assert(MY_HOME_HUB_ID > 0 && MY_HOME_HUB_ID < BP_ID_BROADCAST
              && (MY_HOME_HUB_ID & 0x000F) == 0,
              "MY_HOME_HUB_ID must be a physical hub ID (nonzero multiple of 16)");
