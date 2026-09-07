#include "bluepaws/hub_settings.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bluepaws::hub {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusMetres = 6371008.8;

double radians(double degrees) {
    return degrees * kPi / 180.0;
}

bool validCoordinate(map::GeoPoint point) {
    return std::isfinite(point.latitude) && std::isfinite(point.longitude) &&
           point.latitude >= -90.0 && point.latitude <= 90.0 &&
           point.longitude >= -180.0 && point.longitude <= 180.0;
}

}  // namespace

Settings defaultSettings() {
    Settings settings{};
    sanitize(settings);
    return settings;
}

bool validSsid(const char *value) {
    if (value == nullptr) return false;
    const std::size_t length = std::strlen(value);
    return length > 0 && length < kWifiSsidBytes;
}

bool validPassword(const char *value, bool allow_empty) {
    if (value == nullptr) return false;
    const std::size_t length = std::strlen(value);
    return (allow_empty && length == 0) ||
           (length >= 8 && length < kWifiPasswordBytes);
}

bool validAccessPointPassword(const char *value) {
    return validPassword(value, true);
}

void sanitize(Settings &settings) {
    settings.primary.ssid[kWifiSsidBytes - 1] = '\0';
    settings.primary.password[kWifiPasswordBytes - 1] = '\0';
    settings.secondary.ssid[kWifiSsidBytes - 1] = '\0';
    settings.secondary.password[kWifiPasswordBytes - 1] = '\0';
    settings.access_point_ssid[kWifiSsidBytes - 1] = '\0';
    settings.access_point_password[kWifiPasswordBytes - 1] = '\0';

    if (!validSsid(settings.access_point_ssid)) {
        std::strncpy(settings.access_point_ssid, "BluePaws-Hub",
                     sizeof(settings.access_point_ssid) - 1);
    }
    if (!validAccessPointPassword(settings.access_point_password)) {
        settings.access_point_password[0] = '\0';
    }
    if (!validPassword(settings.primary.password)) settings.primary.password[0] = '\0';
    if (!validPassword(settings.secondary.password)) settings.secondary.password[0] = '\0';

    settings.overview_timeout_seconds = std::clamp<uint16_t>(
        settings.overview_timeout_seconds, 15, 3600);
    settings.dim_timeout_seconds = std::clamp<uint16_t>(
        settings.dim_timeout_seconds, settings.overview_timeout_seconds, 7200);
    settings.screen_off_timeout_seconds = std::clamp<uint16_t>(
        settings.screen_off_timeout_seconds, settings.dim_timeout_seconds, 14400);
    settings.dim_brightness_percent = std::clamp<uint8_t>(
        settings.dim_brightness_percent, 1, 50);
    settings.brightness_percent = std::clamp<uint8_t>(
        settings.brightness_percent, 10, 100);
    settings.volume_percent = std::clamp<uint8_t>(settings.volume_percent, 0, 100);
}

RelativePosition relativePosition(map::GeoPoint origin, map::GeoPoint target) {
    RelativePosition result{};
    if (!validCoordinate(origin) || !validCoordinate(target)) return result;

    const double lat1 = radians(origin.latitude);
    const double lat2 = radians(target.latitude);
    const double delta_lat = lat2 - lat1;
    const double delta_lon = radians(target.longitude - origin.longitude);
    const double sin_lat = std::sin(delta_lat / 2.0);
    const double sin_lon = std::sin(delta_lon / 2.0);
    const double haversine = sin_lat * sin_lat +
        std::cos(lat1) * std::cos(lat2) * sin_lon * sin_lon;
    result.distance_metres = kEarthRadiusMetres * 2.0 *
        std::atan2(std::sqrt(haversine), std::sqrt(std::max(0.0, 1.0 - haversine)));

    const double y = std::sin(delta_lon) * std::cos(lat2);
    const double x = std::cos(lat1) * std::sin(lat2) -
        std::sin(lat1) * std::cos(lat2) * std::cos(delta_lon);
    result.bearing_degrees = std::fmod(std::atan2(y, x) * 180.0 / kPi + 360.0, 360.0);
    const unsigned clock_sector = static_cast<unsigned>(
        std::floor((result.bearing_degrees + 15.0) / 30.0)) % 12U;
    result.clock_hour = static_cast<uint8_t>(clock_sector == 0 ? 12 : clock_sector);
    constexpr const char *kCardinals[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const unsigned cardinal_index = static_cast<unsigned>(
        std::floor((result.bearing_degrees + 22.5) / 45.0)) % 8U;
    result.cardinal = kCardinals[cardinal_index];
    result.valid = true;
    return result;
}

}  // namespace bluepaws::hub
