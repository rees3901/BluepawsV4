#pragma once

#include <cstdint>

namespace bluepaws::testbed {

// Temporary substitutes for sensors that will live on the LoRa/GNSS
// daughterboard. Everything outside this structure remains real runtime data.
// Set HOME_HUB_TESTBED_SIMULATED_TELEMETRY to 0 when the daughterboard adapter
// supplies genuine values.
struct TelemetrySample {
    bool has_position = false;
    double latitude = 0.0;
    double longitude = 0.0;
    bool position_simulated = false;
    bool has_battery = false;
    uint8_t battery_percent = 0;
    bool battery_simulated = false;
};

TelemetrySample telemetrySample(uint32_t uptime_seconds);

}  // namespace bluepaws::testbed
