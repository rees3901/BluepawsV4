#pragma once
#include <stdint.h>

namespace personal {
// Active-high external LED feedback: bounded flash sequences.
class FinderFlash {
public:
    void start(uint32_t now, uint8_t count = 3) {
        active_ = count > 0 && count <= 7; started_ = now;
        period_ = count == 4 ? 200 : count == 7 ? 140 : 230;
        pulse_ = (count == 4 || count == 7) ? 70 : 150;
        duration_ = active_ ? uint16_t((count - 1) * period_ + pulse_) : 0;
    }
    void stop() { active_ = false; }
    bool tick(uint32_t now) {
        if (!active_) return false;
        const uint32_t elapsed = now - started_;
        if (elapsed >= duration_) { active_ = false; return false; }
        return elapsed % period_ < pulse_;
    }
    bool active() const { return active_; }
private:
    bool active_ = false;
    uint32_t started_ = 0;
    uint16_t duration_ = 0, period_ = 230, pulse_ = 150;
};
}
