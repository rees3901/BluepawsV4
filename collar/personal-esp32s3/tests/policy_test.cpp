#include <cassert>
#include <cstdio>
#include "personal_policy.h"

int main() {
    using personal::CommandResult;
    uint8_t p[BP_MAX_PACKET_SIZE] = {}, key[32];
    for (int i = 0; i < 32; ++i) key[i] = uint8_t(i);
    pkt_init(p, 3001, 48, 65432, 1791061200, STATUS_OUT_AND_ABOUT, PROFILE_NORMAL, FLAG_GNSS_VALID, TX_TELEMETRY);
    pkt_set_gps(p, 519059786, -22394294);
    pkt_set_quality(p, 0, 0, 2);
    pkt_set_sat_count(p, 8);
    pkt_add_tlv_u16(p, TLV_FW_VER, 0x0100);
    auto len = personal::sign(p, key);
    assert(personal::wellFormed(p, len));
    for (int i = 0; i < len; ++i) printf("%02x", p[i]);
    puts(""); // Public fixture, independently checked with Python hashlib.
    assert(!personal::wellFormed(p, len - 1));
    assert(!personal::wellFormed(p, 256 + len));
    assert(personal::fixAge(100, 90) == 10);
    assert(personal::fixAge(100, 101) == 65535);
    assert(personal::fixAge(100000, 1) == 65534);
    assert(personal::fixAge(0, 0) == 65535);

    personal::CommandRecord cache[16] = {};
    pkt_init(p, 48, 3001, 41, 0, STATUS_HOME, PROFILE_NORMAL, 0, TX_CONFIG);
    pkt_add_tlv_u8(p, TLV_PROFILE, PROFILE_LOST);
    len = pkt_finalize(p);
    assert(personal::command(p, len, 3001, 48, 1000, cache, 16) == CommandResult::Apply);
    assert(personal::command(p, len, 3002, 48, 1000, cache, 16) == CommandResult::Reject);
    assert(personal::command(p, len, 3001, 16, 1000, cache, 16) == CommandResult::Reject);
    pkt_set_destination(p, BP_ID_BROADCAST);
    assert(personal::command(p, len, 3001, 48, 1000, cache, 16) == CommandResult::Reject);
    pkt_set_destination(p, 3001);
    cache[0].sequence = 41; cache[0].reason = TX_CONFIG; cache[0].seen = 900;
    cache[0].length = pkt_tlv_len(p); memcpy(cache[0].bytes, p + BP_HEADER_SIZE, cache[0].length);
    assert(personal::command(p, len, 3001, 48, 1000, cache, 16) == CommandResult::Duplicate);
    // Simulate NVS round trip, including a reboot with no clock.
    personal::CommandRecord restored[16]; memcpy(restored, cache, sizeof(cache));
    assert(personal::command(p, len, 3001, 48, 0, restored, 16) == CommandResult::Duplicate);
    p[BP_HEADER_SIZE + 2] = PROFILE_NORMAL;
    assert(personal::command(p, len, 3001, 48, 1000, cache, 16) == CommandResult::Reject);
    p[BP_HEADER_SIZE + 2] = PROFILE_DEBUG;
    assert(personal::command(p, len, 3001, 48, 2000, cache, 16) == CommandResult::Reject);
    p[BP_HEADER_SIZE + 1] = 8; // Truncated TLV despite valid outer length.
    assert(!personal::wellFormed(p, len));

    pkt_init(p, 48, 3001, 42, 1000, STATUS_HOME, PROFILE_NORMAL, 0, TX_PING);
    len = pkt_finalize(p);
    assert(personal::command(p, len, 3001, 48, 1601, cache, 16) == CommandResult::Reject);
    assert(personal::command(p, len, 3001, 48, 900, cache, 16) == CommandResult::Reject);
    assert(personal::command(p, len, 3001, 48, 0, cache, 16) == CommandResult::Reject);
    assert(personal::command(p, len, 3001, 48, 1100, cache, 16) == CommandResult::Apply);
    pkt_init(p, 48, 3001, 43, 0, STATUS_HOME, PROFILE_NORMAL, 0, TX_ACK);
    pkt_add_tlv_u16(p, TLV_ACKED_MSG_SEQ_ID, 65432);
    len = pkt_finalize(p);
    assert(personal::receipt(p, len, 3001, 48, 65432));
    assert(!personal::receipt(p, len, 3001, 48, 123));
    assert(personal::command(p, len, 3001, 48, 1100, cache, 16) == CommandResult::Reject);
    assert(CMD_LISTEN_WINDOW_MS == 15000 && UPLINK_MAX_ATTEMPTS == 2);
    puts("PASS: packet bounds, routing, commands, durable duplicates, freshness and ACK separation");
}
