#pragma once

namespace bluepaws::mdns {

// Advertises bluepaws.local on the active Wi-Fi interface, including Home mode.
bool start_mdns();

}  // namespace bluepaws::mdns
