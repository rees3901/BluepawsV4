#include "bluepaws/cat_store.h"

#include <cstring>

namespace bluepaws {

CatRecord *CatStore::find(uint16_t device_id) {
    for (std::size_t i = 0; i < count_; ++i) {
        if (cats_[i].device_id == device_id) return &cats_[i];
    }
    return nullptr;
}

const CatRecord *CatStore::find(uint16_t device_id) const {
    for (std::size_t i = 0; i < count_; ++i) {
        if (cats_[i].device_id == device_id) return &cats_[i];
    }
    return nullptr;
}

CatRecord *CatStore::at(std::size_t index) {
    return index < count_ ? &cats_[index] : nullptr;
}

const CatRecord *CatStore::at(std::size_t index) const {
    return index < count_ ? &cats_[index] : nullptr;
}

ApplyResult CatStore::apply(const CatTelemetry &telemetry) {
    if (telemetry.device_id == 0) return ApplyResult::InvalidDevice;

    CatRecord *record = find(telemetry.device_id);
    ApplyResult result = ApplyResult::Updated;
    if (record == nullptr) {
        if (count_ == cats_.size()) return ApplyResult::CapacityReached;
        record = &cats_[count_++];
        *record = {};
        record->device_id = telemetry.device_id;
        result = ApplyResult::Added;
    }

    record->latest = telemetry;
    if (telemetry.position_valid) {
        record->last_valid_latitude_e7 = telemetry.latitude_e7;
        record->last_valid_longitude_e7 = telemetry.longitude_e7;
        record->last_position_observed_at = telemetry.observed_at;
        record->has_position = true;
    }
    return result;
}

bool CatStore::setName(uint16_t device_id, const char *name) {
    CatRecord *record = find(device_id);
    if (record == nullptr || name == nullptr) return false;
    std::strncpy(record->name, name, sizeof(record->name) - 1);
    record->name[sizeof(record->name) - 1] = '\0';
    return true;
}

void CatStore::clear() {
    cats_ = {};
    count_ = 0;
}

}  // namespace bluepaws
