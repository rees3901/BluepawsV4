#pragma once
#include <stdint.h>

namespace personal {
// Active finder supplies its own tone: only timed trigger pulses are generated.
class FinderChirp {
public:
    void start(uint32_t now, uint8_t count = 3) {
        active_ = count > 0 && count <= 3; started_ = now;
        duration_ = active_ ? uint16_t(count * 230 - 80) : 0;
    }
    void stop() { active_ = false; }
    bool tick(uint32_t now) {
        if (!active_) return false;
        const uint32_t elapsed = now - started_;
        if (elapsed >= duration_) { active_ = false; return false; }
        return elapsed % 230 < 150;
    }
    bool active() const { return active_; }
private:
    bool active_ = false;
    uint32_t started_ = 0;
    uint16_t duration_ = 0;
};
}
