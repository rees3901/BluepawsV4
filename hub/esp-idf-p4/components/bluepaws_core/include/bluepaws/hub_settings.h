#pragma once

#include "bluepaws/map_engine.h"

#include <cstddef>
#include <cstdint>

namespace bluepaws::hub {

constexpr std::size_t kWifiSsidBytes = 33;
constexpr std::size_t kWifiPasswordBytes = 65;
constexpr std::size_t kHubDisplayNameBytes = 33;
constexpr std::size_t kHubEmojiBytes = 9;
constexpr std::size_t kHubMarkerColourBytes = 8;
constexpr uint32_t kControlPollPendingMs = 5000U;
constexpr uint32_t kControlPollIdleMs = 30000U;
constexpr uint32_t kControlPollFailureInitialMs = 60000U;
constexpr uint32_t kControlPollFailureMaximumMs = 300000U;

enum class CommunicationsMode : uint8_t {
    Home = 0,
    Portable = 1,
    OffGrid = 2,
};

enum class ReportingProfile : uint8_t {
    PowerSave = 0,
    Normal = 1,
    Active = 2,
};

struct WifiNetwork {
    char ssid[kWifiSsidBytes]{};
    char password[kWifiPasswordBytes]{};
};

struct Settings {
    WifiNetwork primary{};
    WifiNetwork secondary{};
    char access_point_ssid[kWifiSsidBytes]{"BluePaws.local_IP:192.168.4.1"};
    char access_point_password[kWifiPasswordBytes]{};
    CommunicationsMode communications_mode = CommunicationsMode::Home;
    bool bluetooth_enabled = true;
    ReportingProfile reporting_profile = ReportingProfile::Normal;
    uint64_t cloud_settings_revision = 0;
    char display_name[kHubDisplayNameBytes]{};
    char home_emoji[kHubEmojiBytes]{};
    char portable_emoji[kHubEmojiBytes]{};
    char marker_colour[kHubMarkerColourBytes]{};
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
const char *communicationsModeName(CommunicationsMode mode);
const char *reportingProfileName(ReportingProfile profile);
uint32_t reportingIntervalMs(ReportingProfile profile);
uint32_t controlPollIntervalMs(bool pending, uint8_t consecutive_failures);
RelativePosition relativePosition(map::GeoPoint origin, map::GeoPoint target);

}  // namespace bluepaws::hub
