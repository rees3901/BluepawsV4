#include "bluepaws/cat_simulator.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace bluepaws {
namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kMetresPerLatitudeDegree = 111320.0;
constexpr std::array<const char *, kMaximumCats> kNames{
    "Podge", "Carrie", "Charlie", "Mabel", "Jasper", "Luna", "Milo", "Willow",
};

}  // namespace

CatSimulator::CatSimulator(map::GeoPoint origin) : origin_(map::normalize(origin)) {}

void CatSimulator::reset(map::GeoPoint origin, uint32_t now_ms) {
    origin_ = map::normalize(origin);
    started_at_ms_ = now_ms;
}

void CatSimulator::update(uint32_t now_ms, CatStore &store) const {
    const double elapsed_seconds = static_cast<uint32_t>(now_ms - started_at_ms_) / 1000.0;
    const double longitude_metres = kMetresPerLatitudeDegree *
        std::max(0.1, std::cos(origin_.latitude * kPi / 180.0));

    for (std::size_t i = 0; i < kMaximumCats; ++i) {
        const double phase = i * (2.0 * kPi / kMaximumCats);
        const double radius_metres = 35.0 + i * 17.0;
        const double angle = phase + elapsed_seconds * (0.004 + i * 0.00035);
        const double north_metres = std::sin(angle) * radius_metres;
        const double east_metres = std::cos(angle) * radius_metres;
        const uint32_t cycle = (now_ms / 60000U + static_cast<uint32_t>(i) * 3U) % 45U;

        CatTelemetry telemetry{};
        telemetry.device_id = static_cast<uint16_t>(1001 + i);
        telemetry.sequence = now_ms / 1000U;
        telemetry.latitude_e7 = static_cast<int32_t>(std::lround(
            (origin_.latitude + north_metres / kMetresPerLatitudeDegree) * 1.0e7));
        telemetry.longitude_e7 = static_cast<int32_t>(std::lround(
            (origin_.longitude + east_metres / longitude_metres) * 1.0e7));
        telemetry.battery_percent = static_cast<uint8_t>(95U - cycle);
        telemetry.battery_mv = static_cast<uint16_t>(3600U + telemetry.battery_percent * 6U);
        telemetry.rssi = static_cast<int16_t>(-72 - static_cast<int>(i) * 4);
        telemetry.snr = 9.0F - static_cast<float>(i) * 0.75F;
        telemetry.observed_at = now_ms / 1000U;
        telemetry.received_at_ms = now_ms;
        telemetry.position_valid = true;
        telemetry.source = TelemetrySource::Simulation;
        store.apply(telemetry);
        store.setName(telemetry.device_id, kNames[i]);
    }
}

}  // namespace bluepaws
