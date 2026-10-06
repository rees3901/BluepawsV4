#pragma once
#include <stdint.h>
namespace personal {
constexpr uint8_t LedActionTlv = 0xFA, LedDurationTlv = 0xFB, LedIntervalTlv = 0xFC;
struct LedSchedule {
    uint32_t until = 0, next = 0;
    bool suppressLost = false;
    uint16_t sequence = 0;
    uint16_t intervalSeconds = 60;
    bool accept(uint16_t incoming) {
        if (!incoming || (sequence && uint16_t(incoming - sequence) >= 32768) || incoming == sequence) return false;
        sequence = incoming; return true;
    }
    bool repeat(uint32_t now, uint16_t seconds, uint16_t interval) {
        if (!now || seconds < 10 || seconds > 14400 || interval < 10 || interval > 600) return false;
        until = now + seconds; next = now; intervalSeconds = interval; suppressLost = true; return true;
    }
    void stop() { until = next = 0; suppressLost = true; }
    void enterLost() { suppressLost = false; next = 0; }
    bool due(uint32_t now, bool lost) {
        if (!now) return false;
        if (until && now >= until) { until = next = 0; }
        if (!until && (!lost || suppressLost)) return false;
        if (next && now < next) return false;
        next = now + (until ? intervalSeconds : 60); return true;
    }
    uint32_t wake(uint32_t now, bool lost) const {
        if (!now || (!until && (!lost || suppressLost))) return 0;
        if (until && now >= until) return 0;
        return next > now ? next : now + 1;
    }
};
}
