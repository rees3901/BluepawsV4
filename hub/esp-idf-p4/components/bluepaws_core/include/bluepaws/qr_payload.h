#pragma once

#include "bluepaws/hub_settings.h"

#include <cstddef>
#include <cstdint>

namespace bluepaws::qr {

enum class PayloadType : uint8_t {
    Unsupported,
    Wifi,
    Collar,
};

enum class WifiSecurity : uint8_t {
    Open,
    Wep,
    Wpa,
};

struct WifiCredentials {
    char ssid[hub::kWifiSsidBytes]{};
    char password[hub::kWifiPasswordBytes]{};
    WifiSecurity security = WifiSecurity::Open;
    bool hidden = false;
};

struct ParsedPayload {
    PayloadType type = PayloadType::Unsupported;
    WifiCredentials wifi{};
    char collar_id[65]{};
};

// Parses the common WIFI:T:WPA;S:network;P:password;H:false;; QR format.
// Backslash escapes for semicolon, colon, comma and backslash are supported.
bool parse(const char *payload, ParsedPayload &parsed);

}  // namespace bluepaws::qr
