#pragma once

namespace bluepaws::captive_dns {

// Starts one low-overhead wildcard IPv4 DNS responder for the lifetime of the
// firmware. It is advertised only by the SoftAP DHCP server.
bool start();

// Advertises bluepaws.local over multicast DNS on whichever Wi-Fi interface
// is active. Kept separate from captive DNS so it also works in Home mode.
bool start_mdns();

}  // namespace bluepaws::captive_dns
