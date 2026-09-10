#pragma once

namespace bluepaws::captive_dns {

// Starts one low-overhead wildcard IPv4 DNS responder for the lifetime of the
// firmware. It is advertised only by the SoftAP DHCP server.
bool start();

}  // namespace bluepaws::captive_dns
