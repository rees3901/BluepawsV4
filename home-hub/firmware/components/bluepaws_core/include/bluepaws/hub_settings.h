#pragma once

#include "bluepaws/map_engine.h"

#include <cstddef>
#include <cstdint>

namespace bluepaws::hub {

constexpr std::size_t kWifiSsidBytes = 33;
constexpr std::size_t kWifiPasswordBytes = 65;

struct WifiNetwork {
    char ssid[kWifiSsidBytes]{};
    char password[kWifiPasswordBytes]{};
};

struct Settings {
    WifiNetwork primary{};
    WifiNetwork secondary{};
    char access_point_ssid[kWifiSsidBytes]{"BluePaws-Hub"};
    char access_point_password[kWifiPasswordBytes]{};
    bool access_point_enabled = true;
    uint16_t overview_timeout_seconds = 120;
    uint16_t dim_timeout_seconds = 180;
    uint16_t screen_off_timeout_seconds = 300;
    uint8_t dim_brightness_percent = 20;
    uint8_t brightness_percent = 80;
    uint8_t volume_percent = 60;
};

struct RelativePosition {
    double distance_metres = 0.0;
    double bearing_degrees = 0.0;
    uint8_t clock_hour = 12;
    const char *cardinal = "N";
    bool valid = false;
};

Settings defaultSettings();
void sanitize(Settings &settings);
bool validSsid(const char *value);
bool validPassword(const char *value, bool allow_empty = true);
bool validAccessPointPassword(const char *value);
RelativePosition relativePosition(map::GeoPoint origin, map::GeoPoint target);

}  // namespace bluepaws::hub
