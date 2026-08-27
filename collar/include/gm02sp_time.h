#ifndef BLUEPAWS_GM02SP_TIME_H
#define BLUEPAWS_GM02SP_TIME_H

#include <stddef.h>
#include <stdint.h>

namespace bluepaws {
namespace gm02sp {

// Foundation only: no modem power control, UART ownership or automatic clock
// changes. The future single-owner modem task sends this with a CR terminator.
constexpr const char* kReadGnssClock = "AT+LPGNSSUTCTIME?\r";
constexpr uint32_t kClockQueryTimeoutMs = 3000;

enum class TimeSource : uint8_t {
    Unknown,
    ModemGnssClock,  // Can be software-seeded; NOT proof of satellite lock.
    FixTimestamp    // Caller must validate the complete fix and its freshness.
};

struct TimeSample {
    uint32_t unixSeconds = 0;
    TimeSource source = TimeSource::Unknown;
};

// Strict UTC conversion, independent of host TZ/BST and time_t width.
// Accepts YYYY-MM-DDTHH:MM:SS, optional fractional seconds and optional Z.
// Bounds: 2023-01-01 through the TLV uint32 Unix maximum (2106-02-07).
// Failed parsing leaves the output unchanged. No local-time offsets accepted.
bool parseUtc(const char* text, uint32_t& unixSeconds);
bool parseGnssClockLine(const char* line, TimeSample& sample);

// Extracts time only from +LPGNSSFIXREADY. Not a position/fix validator.
// Feed complete, bounded lines after the modem's UART dispatcher frames them.
bool parseFixTimestamp(const char* line, TimeSample& sample);

enum class QueryState : uint8_t {
    Idle, Waiting, Ready, NoClock, InvalidReply, ModemError, Timeout
};

class ClockQuery {
public:
    // False when already waiting or given an invalid timeout. A successful
    // begin clears any previous sample; caller then writes kReadGnssClock.
    bool begin(uint32_t nowMs, uint32_t timeoutMs = kClockQueryTimeoutMs);
    // Returns true only for this transaction's response/echo/final lines.
    // Unrelated URCs return false and MUST still reach their normal handlers.
    // Data is published only after both a valid clock line and terminal OK.
    bool onLine(const char* line, uint32_t nowMs);
    void poll(uint32_t nowMs);
    QueryState state() const { return state_; }
    bool sample(TimeSample& result) const;

private:
    QueryState state_ = QueryState::Idle;
    QueryState pending_ = QueryState::InvalidReply;
    TimeSample candidate_{};
    uint32_t startedMs_ = 0;
    uint32_t timeoutMs_ = 0;
    bool sawClock_ = false;
};

// Optional RAM clock anchor for the future modem task. Never restore this from
// a persisted position timestamp: elapsed power-off time would be unknown.
// Caller provides a monotonic 64-bit time that continues across system-on sleep
// (not raw wrapping millis()). Synchronize access if shared between tasks.
class TimeAnchor {
public:
    bool set(const TimeSample& sample, uint64_t nowMs);
    bool read(uint64_t nowMs, uint32_t maxAgeSeconds, TimeSample& result) const;
    void clear() { sample_ = TimeSample{}; }

private:
    TimeSample sample_{};
    uint64_t sampledMs_ = 0;
};

} // namespace gm02sp
} // namespace bluepaws
#endif
