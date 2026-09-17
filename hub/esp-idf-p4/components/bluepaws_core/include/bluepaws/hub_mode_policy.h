#pragma once

#include "bluepaws/hub_settings.h"

namespace bluepaws::hub {

// Pure policy shared by the runtime and host-side tests. Network 0 is the
// primary/home SSID and network 1 is the secondary/portable SSID.
constexpr bool modeAllowsNetwork(CommunicationsMode requested, unsigned index) {
    if (requested == CommunicationsMode::OffGrid || index > 1) return false;
    return requested == CommunicationsMode::Home || index == 1;
}

constexpr unsigned preferredNetwork(CommunicationsMode requested) {
    return requested == CommunicationsMode::Portable ? 1U : 0U;
}

constexpr CommunicationsMode effectiveModeForNetwork(unsigned index) {
    return index == 1 ? CommunicationsMode::Portable : CommunicationsMode::Home;
}

}  // namespace bluepaws::hub
