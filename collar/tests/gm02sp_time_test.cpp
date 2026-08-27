#include "gm02sp_time.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace bluepaws::gm02sp;

static const char* clockReply = "+LPGNSSUTCTIME: \"2026-08-27T09:00:00\"";
static const uint32_t epoch = 1787821200UL;

static void utcTests() {
    uint32_t result = 7;
    assert(parseUtc("2026-08-27T09:00:00", result) && result == epoch);
    assert(parseUtc("2026-08-27T09:00:00.123456789Z", result) && result == epoch);
    assert(parseUtc("2024-02-29T00:00:00Z", result) && result == 1709164800UL);
    assert(parseUtc("2023-01-01T00:00:00", result) && result == 1672531200UL);
    assert(parseUtc("2038-01-19T03:14:08", result) && result == 2147483648UL);
    assert(parseUtc("2106-02-07T06:28:15", result) && result == UINT32_MAX);
    const char* invalid[] = {
        nullptr, "", "NO_CLOCK_DEFINED", "1970-01-01T00:00:00",
        "2022-12-31T23:59:59", "2026-02-29T00:00:00", "2100-02-29T00:00:00",
        "2026-00-01T00:00:00", "2026-13-01T00:00:00", "2026-04-31T00:00:00",
        "2026-01-00T00:00:00", "2026-01-01T24:00:00", "2026-01-01T00:60:00",
        "2026-01-01T00:00:60", "2026-01-01 00:00:00", "2026-1-01T00:00:00",
        "2026-01-01T00:00:00+01:00", "2026-01-01T00:00:00garbage",
        "2026-01-01T00:00:00.Z", "2026-01-01T00:00:00.1234567890",
        "2106-02-07T06:28:16", "2107-01-01T00:00:00",
        "2026-01-01T00:00:00.1234567890123456789012345678901234567890"
    };
    for (const char* value : invalid) {
        result = 123;
        assert(!parseUtc(value, result) && result == 123);
    }
}

static void responseTests() {
    TimeSample sample;
    assert(parseGnssClockLine(clockReply, sample));
    assert(sample.unixSeconds == epoch && sample.source == TimeSource::ModemGnssClock);
    const char* invalid[] = {
        "OK", "+CCLK: \"26/08/27,09:00:00+00\"", "+LPGNSSUTCTIME: \"NO_CLOCK_DEFINED\"",
        "+LPGNSSUTCTIME: 2026-08-27T09:00:00", "+LPGNSSUTCTIME: \"2026-08-27T09:00:00",
        "+LPGNSSUTCTIME: \"2026-08-27T09:00:00\",extra"
    };
    for (const char* line : invalid) {
        assert(!parseGnssClockLine(line, sample));
        assert(sample.unixSeconds == epoch && sample.source == TimeSource::ModemGnssClock);
    }
    // Synthetic fixture, not a claim of captured hardware output.
    assert(parseFixTimestamp("+LPGNSSFIXREADY: 1,\"2026-08-27T09:00:00\",12,\"5.0\",\"51.9\",\"-2.2\",\"30\",\"0\",\"0\",\"0\"", sample));
    assert(sample.unixSeconds == epoch && sample.source == TimeSource::FixTimestamp);
    assert(!parseFixTimestamp("+LPGNSSFIXREADY: ,\"2026-08-27T09:00:00\",12", sample));
    assert(!parseFixTimestamp("+LPGNSSFIXREADY: 65536,\"2026-08-27T09:00:00\",12", sample));
    assert(!parseFixTimestamp("+LPGNSSFIXREADY: 1,\"2026-08-27T09:00:00\"", sample));
    assert(!parseFixTimestamp("+LPGNSSFIXREADY: 1,\"2026-08-27T09:00:00\",", sample));
}

static void transactionTests() {
    ClockQuery query;
    TimeSample sample;
    assert(strcmp(kReadGnssClock, "AT+LPGNSSUTCTIME?\r") == 0);
    assert(query.state() == QueryState::Idle && !query.sample(sample));
    assert(!query.begin(0, 0) && !query.begin(0, 60001));
    assert(query.begin(100));
    assert(!query.begin(101));
    assert(query.onLine("AT+LPGNSSUTCTIME?", 101));
    assert(!query.onLine("+CEREG: 5", 102));
    assert(!query.onLine("+LPGNSSFIXREADY: 1,\"2026-08-27T09:00:00\",12", 103));
    assert(query.onLine(clockReply, 104));
    assert(!query.sample(sample)); // Data without OK must not be trusted.
    assert(query.onLine("OK", 105));
    assert(query.sample(sample) && sample.unixSeconds == epoch);
    assert(!query.onLine("ERROR", 106)); // Completed transaction cannot change.

    assert(query.begin(200));
    assert(!query.sample(sample)); // Never leak an earlier successful result.
    query.onLine("+LPGNSSUTCTIME: \"NO_CLOCK_DEFINED\"", 201);
    query.onLine("OK", 202);
    assert(query.state() == QueryState::NoClock && !query.sample(sample));

    const char* errors[] = {"ERROR", "+CME ERROR: 3", "+CMS ERROR: 500"};
    for (const char* error : errors) {
        assert(query.begin(300));
        query.onLine(clockReply, 301);
        assert(query.onLine(error, 302));
        assert(query.state() == QueryState::ModemError && !query.sample(sample));
    }
    assert(query.begin(400));
    query.onLine("OK", 401);
    assert(query.state() == QueryState::InvalidReply);
    assert(query.begin(500));
    query.onLine("+LPGNSSUTCTIME: \"bad\"", 501);
    query.onLine("OK", 502);
    assert(query.state() == QueryState::InvalidReply);
    assert(query.begin(600));
    query.onLine(clockReply, 601);
    query.onLine(clockReply, 602);
    query.onLine("OK", 603);
    assert(query.state() == QueryState::InvalidReply);
    assert(query.begin(700));
    query.onLine(clockReply, 701);
    query.poll(3700);
    assert(query.state() == QueryState::Timeout && !query.sample(sample));
    assert(!query.onLine("OK", 3701));
    assert(query.begin(UINT32_MAX - 99, 200));
    query.poll(99);
    assert(query.state() == QueryState::Waiting);
    query.poll(100);
    assert(query.state() == QueryState::Timeout); // millis rollover
}

static void anchorTests() {
    TimeAnchor anchor;
    TimeSample sample, output;
    assert(!anchor.read(1000, 60, output));
    assert(!anchor.set(sample, 1000));
    assert(parseGnssClockLine(clockReply, sample));
    assert(anchor.set(sample, 1000));
    assert(anchor.read(61000, 60, output) && output.unixSeconds == epoch + 60);
    assert(output.source == TimeSource::ModemGnssClock);
    assert(!anchor.read(61001, 60, output));
    assert(!anchor.read(999, 60, output)); // Monotonic clock reset
    sample.unixSeconds = UINT32_MAX;
    assert(anchor.set(sample, 0));
    assert(!anchor.read(1000, 60, output)); // TLV timestamp overflow
    anchor.clear();
    assert(!anchor.read(0, 60, output));
}

int main() {
    utcTests();
    responseTests();
    transactionTests();
    anchorTests();
    puts("GM02SP time foundation: UTC, responses, transactions and holdover PASS");
}
