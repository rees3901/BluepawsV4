#pragma once
#include <bp_protocol.h>
#include <bp_config.h>
#include <bp_hmac_sha256.h>
#include <stddef.h>
#include "led_schedule.h"

namespace personal {
constexpr uint32_t COMMAND_CACHE_SECONDS = 900;
struct CommandRecord {
    uint16_t sequence = 0;
    uint8_t reason = 0;
    uint8_t length = 0;
    uint32_t seen = 0;
    uint8_t bytes[BP_MAX_TLV_SIZE] = {};
};
enum class CommandResult { Reject, Apply, Duplicate };

inline bool wellFormed(const uint8_t* packet, size_t size) {
    if (size > BP_MAX_PACKET_SIZE || !pkt_validate_structure(packet, uint8_t(size))) return false;
    size_t p = BP_HEADER_SIZE, end = p + pkt_tlv_len(packet);
    while (p < end) {
        if (p + 2 > end || p + 2 + packet[p + 1] > end) return false;
        p += 2 + packet[p + 1];
    }
    return p == end;
}

inline bool addressed(const uint8_t* p, size_t n, uint16_t collar, uint16_t hub) {
    return wellFormed(p, n) && pkt_source_id(p) == hub && pkt_destination_id(p) == collar;
}

// Current canonical hub emits timestamp=0 and an unsigned auth trailer.
// Timestamped commands can be age checked; zero timestamps rely on hub expiry.
// Address checking is NOT cryptographic authentication.
inline CommandResult command(const uint8_t* p, size_t n, uint16_t collar, uint16_t hub,
                             uint32_t now, const CommandRecord* cache, size_t count, bool ledSupport = false) {
    if (!addressed(p, n, collar, hub) || !pkt_msg_seq(p)) return CommandResult::Reject;
    const auto reason = pkt_tx_reason(p);
    if (reason != TX_CONFIG && reason != TX_PING && reason != TX_INTERRUPT) return CommandResult::Reject;
    uint8_t profile = PROFILE_UNKNOWN;
    if (reason == TX_CONFIG) {
        const uint8_t* value; uint8_t len;
        uint8_t ledAction = 255;
        if (pkt_tlv_find(p, LedActionTlv, &value, &len)) {
            if (len != 1 || value[0] > 2) return CommandResult::Reject;
            ledAction = value[0];
            if (!ledSupport) return CommandResult::Reject; // Never ACK an unsupported output.
            uint16_t duration = 0, interval = 0;
            if (!pkt_tlv_find(p, LedDurationTlv, &value, &len) || len != 2 || !pkt_tlv_get_u16(p, LedDurationTlv, &duration)) return CommandResult::Reject;
            if (!pkt_tlv_find(p, LedIntervalTlv, &value, &len) || len != 2 || !pkt_tlv_get_u16(p, LedIntervalTlv, &interval)) return CommandResult::Reject;
            if (interval != 60 || (ledAction == 1 && (!now || duration < 60 || duration > 3600)) || (ledAction != 1 && duration)) return CommandResult::Reject;
            if (pkt_tlv_find(p, TLV_PROFILE, &value, &len)) return CommandResult::Reject;
        } else {
            if (!pkt_tlv_find(p, TLV_PROFILE, &value, &len) || len != 1) return CommandResult::Reject;
            profile = value[0];
            if (profile > PROFILE_LOST) return CommandResult::Reject; // debug is not a live profile
        }
    }
    const uint32_t sent = pkt_time_unix(p);
    if (sent && (!now || sent > now + 30 || now > sent + 600)) return CommandResult::Reject;
    for (size_t i = 0; i < count; ++i) {
        const auto& c = cache[i];
        // Unknown clock cannot safely age out a durable duplicate record.
        if (c.sequence != pkt_msg_seq(p) || (now && c.seen && now >= c.seen && now - c.seen > COMMAND_CACHE_SECONDS)) continue;
        return c.reason == reason && c.length == pkt_tlv_len(p) &&
            memcmp(c.bytes, p + BP_HEADER_SIZE, c.length) == 0
            ? CommandResult::Duplicate : CommandResult::Reject;
    }
    return CommandResult::Apply;
}

inline uint16_t fixAge(uint32_t now, uint32_t fix) {
    if (!now || !fix || now < fix) return UINT16_MAX;
    const uint32_t age = now - fix;
    return age > 65534 ? 65534 : uint16_t(age);
}

inline uint8_t sign(uint8_t* p, const uint8_t* key) {
    const uint8_t size = pkt_finalize(p);
    bp_hmac_sha256_truncated8(key, 32, p, size - BP_AUTH_TAG_SIZE, p + size - BP_AUTH_TAG_SIZE);
    return size;
}

inline bool receipt(const uint8_t* p, size_t n, uint16_t collar, uint16_t hub, uint16_t seq) {
    uint16_t ack = 0;
    return addressed(p, n, collar, hub) && pkt_tx_reason(p) == TX_ACK &&
           pkt_tlv_get_u16(p, TLV_ACKED_MSG_SEQ_ID, &ack) && ack == seq;
}
} // namespace personal
