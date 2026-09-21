#include "home_hub_testbed_telemetry.h"

#include "home_hub_config.h"

#include <algorithm>
#include <cmath>

namespace bluepaws::testbed {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMetresPerLatitudeDegree = 111320.0;

}  // namespace

TelemetrySample telemetrySample(uint32_t uptime_seconds)
{
    TelemetrySample sample{};
#if HOME_HUB_TESTBED_SIMULATED_TELEMETRY
    // Hold one point for each reporting minute. The golden-angle walk avoids a
    // repetitive circle while the square-root radius keeps every fix inside
    // the requested five-metre disc rather than clustering at its centre.
    const uint32_t step = uptime_seconds / 60U;
    const double phase = static_cast<double>(step) * 2.399963229728653;
    const double radial_fraction = std::sqrt(
        static_cast<double>((step * 37U + 17U) % 101U) / 100.0);
    const double radius_metres = HOME_HUB_TESTBED_RADIUS_METRES * radial_fraction;
    const double north_metres = std::cos(phase) * radius_metres;
    const double east_metres = std::sin(phase) * radius_metres;
    const double longitude_scale = kMetresPerLatitudeDegree * std::max(
        0.1, std::cos(HOME_HUB_TESTBED_LATITUDE * kPi / 180.0));

    sample.has_position = true;
    sample.latitude = HOME_HUB_TESTBED_LATITUDE +
        north_metres / kMetresPerLatitudeDegree;
    sample.longitude = HOME_HUB_TESTBED_LONGITUDE + east_metres / longitude_scale;
    sample.position_simulated = true;

    // A slow 88-96% wave exercises the real battery UI without pretending an
    // ADC exists. The source flag follows the value through cloud storage.
    const double battery_wave = std::sin(static_cast<double>(uptime_seconds) / 1800.0);
    sample.has_battery = true;
    sample.battery_percent = static_cast<uint8_t>(std::clamp(
        static_cast<int>(std::lround(92.0 + 4.0 * battery_wave)), 0, 100));
    sample.battery_simulated = true;
#endif
    return sample;
}

}  // namespace bluepaws::testbed
