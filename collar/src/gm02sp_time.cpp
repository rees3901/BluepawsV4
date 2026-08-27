#include "gm02sp_time.h"

#include <string.h>

namespace bluepaws {
namespace gm02sp {
namespace {
constexpr uint32_t kMinUnix = 1672531200UL; // 2023-01-01 UTC
constexpr const char* kClockPrefix = "+LPGNSSUTCTIME:";
constexpr const char* kFixPrefix = "+LPGNSSFIXREADY:";

bool digit(char c) { return c >= '0' && c <= '9'; }
bool leap(unsigned year) {
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}
unsigned number(const char* text, unsigned count) {
    unsigned result = 0;
    for (unsigned i = 0; i < count; ++i) result = result * 10 + (text[i] - '0');
    return result;
}
const char* skipSpace(const char* text) {
    while (*text == ' ' || *text == '\t') ++text;
    return text;
}
bool starts(const char* text, const char* prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}
// Copy only a short quoted ISO timestamp, never an arbitrary modem line.
bool quotedTime(const char*& text, uint32_t& unixSeconds) {
    text = skipSpace(text);
    if (*text++ != '"') return false;
    char value[40];
    size_t length = 0;
    while (*text && *text != '"') {
        if (length == sizeof(value) - 1) return false;
        value[length++] = *text++;
    }
    if (*text != '"') return false;
    ++text;
    value[length] = '\0';
    return parseUtc(value, unixSeconds);
}
} // namespace

bool parseUtc(const char* text, uint32_t& unixSeconds) {
    if (!text) return false;
    // Bounded scan rather than strlen on arbitrary UART data.
    size_t length = 0;
    while (length < 40 && text[length]) ++length;
    if (length < 19 || length == 40) return false;
    for (size_t i = 0; i < 19; ++i) {
        const char separator = (i == 4 || i == 7) ? '-' :
                               i == 10 ? 'T' : (i == 13 || i == 16) ? ':' : '\0';
        if (separator ? text[i] != separator : !digit(text[i])) return false;
    }
    size_t suffix = 19;
    if (text[suffix] == '.') {
        ++suffix;
        const size_t first = suffix;
        while (digit(text[suffix])) ++suffix;
        if (suffix == first || suffix - first > 9) return false;
    }
    if (text[suffix] == 'Z') ++suffix;
    if (suffix != length) return false;

    const unsigned year = number(text, 4), month = number(text + 5, 2);
    const unsigned day = number(text + 8, 2), hour = number(text + 11, 2);
    const unsigned minute = number(text + 14, 2), second = number(text + 17, 2);
    if (year < 2023 || year > 2106 || month < 1 || month > 12 ||
        hour > 23 || minute > 59 || second > 59) return false;
    const unsigned monthDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const unsigned daysInMonth = monthDays[month - 1] + (month == 2 && leap(year));
    if (day < 1 || day > daysInMonth) return false;
    uint64_t days = 0;
    for (unsigned y = 1970; y < year; ++y) days += leap(y) ? 366 : 365;
    for (unsigned m = 1; m < month; ++m) days += monthDays[m - 1] + (m == 2 && leap(year));
    days += day - 1;
    const uint64_t result = days * 86400 + hour * 3600 + minute * 60 + second;
    if (result > UINT32_MAX) return false;
    unixSeconds = static_cast<uint32_t>(result);
    return true;
}

bool parseGnssClockLine(const char* line, TimeSample& sample) {
    if (!line || !starts(line, kClockPrefix)) return false;
    const char* value = line + strlen(kClockPrefix);
    uint32_t epoch = 0;
    if (!quotedTime(value, epoch) || *skipSpace(value) != '\0') return false;
    sample.unixSeconds = epoch;
    sample.source = TimeSource::ModemGnssClock;
    return true;
}

bool parseFixTimestamp(const char* line, TimeSample& sample) {
    if (!line || !starts(line, kFixPrefix)) return false;
    const char* value = skipSpace(line + strlen(kFixPrefix));
    unsigned id = 0, digits = 0;
    while (digit(*value) && digits < 5) {
        id = id * 10 + (*value++ - '0');
        ++digits;
    }
    value = skipSpace(value);
    if (!digits || id > UINT16_MAX || *value != ',') return false;
    ++value;
    uint32_t epoch = 0;
    if (!quotedTime(value, epoch)) return false;
    value = skipSpace(value);
    if (*value != ',' || !*skipSpace(value + 1)) return false;
    sample.unixSeconds = epoch;
    sample.source = TimeSource::FixTimestamp;
    return true;
}

bool ClockQuery::begin(uint32_t nowMs, uint32_t timeoutMs) {
    if (state_ == QueryState::Waiting || timeoutMs == 0 || timeoutMs > 60000) return false;
    state_ = QueryState::Waiting;
    pending_ = QueryState::InvalidReply;
    candidate_ = TimeSample{};
    startedMs_ = nowMs;
    timeoutMs_ = timeoutMs;
    sawClock_ = false;
    return true;
}

void ClockQuery::poll(uint32_t nowMs) {
    if (state_ == QueryState::Waiting && static_cast<uint32_t>(nowMs - startedMs_) >= timeoutMs_)
        state_ = QueryState::Timeout;
}

bool ClockQuery::onLine(const char* line, uint32_t nowMs) {
    poll(nowMs);
    if (!line || state_ != QueryState::Waiting) return false;
    if (strcmp(line, "AT+LPGNSSUTCTIME?") == 0) return true;
    if (starts(line, kClockPrefix)) {
        if (sawClock_) {
            pending_ = QueryState::InvalidReply; // Ambiguous duplicate response.
        } else if (strcmp(skipSpace(line + strlen(kClockPrefix)), "\"NO_CLOCK_DEFINED\"") == 0) {
            pending_ = QueryState::NoClock;
        } else {
            pending_ = parseGnssClockLine(line, candidate_) ? QueryState::Ready : QueryState::InvalidReply;
        }
        sawClock_ = true;
        return true;
    }
    if (strcmp(line, "OK") == 0) {
        state_ = pending_;
        return true;
    }
    if (strcmp(line, "ERROR") == 0 || starts(line, "+CME ERROR:") || starts(line, "+CMS ERROR:")) {
        state_ = QueryState::ModemError;
        return true;
    }
    return false;
}

bool ClockQuery::sample(TimeSample& result) const {
    if (state_ != QueryState::Ready) return false;
    result = candidate_;
    return true;
}

bool TimeAnchor::set(const TimeSample& sample, uint64_t nowMs) {
    if (sample.source == TimeSource::Unknown || sample.unixSeconds < kMinUnix) return false;
    sample_ = sample;
    sampledMs_ = nowMs;
    return true;
}

bool TimeAnchor::read(uint64_t nowMs, uint32_t maxAgeSeconds, TimeSample& result) const {
    if (sample_.source == TimeSource::Unknown || nowMs < sampledMs_) return false;
    const uint64_t elapsedMs = nowMs - sampledMs_;
    if (elapsedMs > static_cast<uint64_t>(maxAgeSeconds) * 1000) return false;
    const uint64_t epoch = sample_.unixSeconds + elapsedMs / 1000;
    if (epoch > UINT32_MAX) return false;
    result = sample_;
    result.unixSeconds = static_cast<uint32_t>(epoch);
    return true;
}

} // namespace gm02sp
} // namespace bluepaws
