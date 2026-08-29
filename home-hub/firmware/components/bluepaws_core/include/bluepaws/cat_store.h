#ifndef BLUEPAWS_HOME_HUB_CAT_STORE_H
#define BLUEPAWS_HOME_HUB_CAT_STORE_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace bluepaws {

constexpr std::size_t kMaximumCats = 8;
constexpr std::size_t kCatNameBytes = 25;

enum class TelemetrySource : uint8_t {
    Simulation,
    LoRa,
    Cloud,
    Restored,
};

struct CatTelemetry {
    uint16_t device_id = 0;
    uint32_t sequence = 0;
    int32_t latitude_e7 = 0;
    int32_t longitude_e7 = 0;
    uint16_t battery_mv = 0;
    uint8_t battery_percent = 0;
    int16_t rssi = 0;
    float snr = 0.0F;
    uint32_t observed_at = 0;
    uint32_t received_at_ms = 0;
    bool position_valid = false;
    TelemetrySource source = TelemetrySource::Simulation;
};

struct CatRecord {
    uint16_t device_id = 0;
    char name[kCatNameBytes]{};
    CatTelemetry latest{};
    int32_t last_valid_latitude_e7 = 0;
    int32_t last_valid_longitude_e7 = 0;
    uint32_t last_position_observed_at = 0;
    bool has_position = false;
};

enum class ApplyResult : uint8_t {
    Added,
    Updated,
    InvalidDevice,
    CapacityReached,
};

class CatStore {
public:
    ApplyResult apply(const CatTelemetry &telemetry);
    bool setName(uint16_t device_id, const char *name);

    CatRecord *find(uint16_t device_id);
    const CatRecord *find(uint16_t device_id) const;
    CatRecord *at(std::size_t index);
    const CatRecord *at(std::size_t index) const;
    std::size_t size() const { return count_; }
    void clear();

private:
    std::array<CatRecord, kMaximumCats> cats_{};
    std::size_t count_ = 0;
};

}  // namespace bluepaws

#endif
