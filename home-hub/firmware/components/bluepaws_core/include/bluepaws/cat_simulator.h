#ifndef BLUEPAWS_HOME_HUB_CAT_SIMULATOR_H
#define BLUEPAWS_HOME_HUB_CAT_SIMULATOR_H

#include "bluepaws/cat_store.h"
#include "bluepaws/map_engine.h"

#include <cstdint>

namespace bluepaws {

class CatSimulator {
public:
    explicit CatSimulator(map::GeoPoint origin);
    void reset(map::GeoPoint origin, uint32_t now_ms = 0);
    void update(uint32_t now_ms, CatStore &store) const;

private:
    map::GeoPoint origin_;
    uint32_t started_at_ms_ = 0;
};

}  // namespace bluepaws

#endif
