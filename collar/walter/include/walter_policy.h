#pragma once
#include <bp_config.h>
#include <bp_hmac_sha256.h>

namespace walter {

struct Decision {
    bool packet;
    bool gnss;
    bool lte;
    uint8_t reason;
};

// Same profile table as WisMesh. No fake radio ACK or fake cloud receipt.
inline Decision decide(bp_profile_t profile, uint32_t cycle, uint32_t homeCycle,
                       bool home, bool boot, bool force, uint32_t sinceLteS) {
    const auto& p = *bp_profile_config(profile);
    if (boot || force) return {true, true, true, uint8_t(boot ? TX_BOOT : TX_INTERRUPT)};
    if (home && profile != PROFILE_LOST) {
        const bool refresh = p.home_gnss_refresh_ratio && homeCycle % p.home_gnss_refresh_ratio == 0;
        const bool checkin = p.wake_checkin_ratio && homeCycle % p.wake_checkin_ratio == 0;
        const bool lte = p.lte_heartbeat_interval_s && sinceLteS >= p.lte_heartbeat_interval_s;
        return {refresh || checkin || lte, refresh, lte, uint8_t(refresh ? TX_TELEMETRY : TX_WAKE_CHECKIN)};
    }
    return {true, true, bool(p.cellular_ratio && cycle % p.cellular_ratio == 0), TX_TELEMETRY};
}

inline uint32_t sleepSeconds(bp_profile_t profile) {
    return profile == PROFILE_LOST ? LOST_MODE_CYCLE_INTERVAL_S : bp_profile_config(profile)->sleep_interval_s;
}

inline bool validUtc(int64_t utc) { return utc >= 1704067200LL && utc <= UINT32_MAX; }

// Reject reset/default modem dates such as 2070. Build time is a plausibility
// bound only, never a substitute timestamp in telemetry or the GNSS fix.
inline bool plausibleUtc(int64_t utc, uint32_t buildUtc) {
    return validUtc(utc) && utc >= int64_t(buildUtc) - 86400 &&
        utc <= int64_t(buildUtc) + 5LL * 366 * 86400;
}

struct Fix {
    bool valid = false;
    int32_t latE7 = 0;
    int32_t lonE7 = 0;
    uint32_t utc = 0;
    uint16_t accuracyM = 0;
    uint8_t satellites = 255;
};

// Zero means no ADC reading on this board, not a measured empty battery.
// Only GNSS_VALID is asserted: Walter does not expose a confirmed 3D-fix field.
inline uint8_t buildPacket(uint8_t* bytes, uint16_t source, uint16_t destination,
                           uint16_t seq, uint32_t utc, bp_profile_t profile,
                           bool home, uint8_t reason, const Fix& fix,
                           bool gpsFailure, bool cellularFailure, uint32_t uptime,
                           const uint8_t key[32]) {
    if (!bp_is_collar_id(source) || !bp_is_hub_id(destination) ||
        !validUtc(utc) || profile > PROFILE_DEBUG || reason > TX_WAKE_CHECKIN) return 0;
    const bool usable = fix.valid && validUtc(fix.utc) &&
        fix.latE7 >= -900000000 && fix.latE7 <= 900000000 &&
        fix.lonE7 >= -1800000000 && fix.lonE7 <= 1800000000;
    const uint32_t age = usable && utc >= fix.utc ? utc - fix.utc : UINT32_MAX;
    const bool wantsFix = reason != TX_WAKE_CHECKIN;
    const bool fresh = wantsFix && usable && age < GPS_STALE_THRESHOLD_S;
    uint8_t flags = home ? FLAG_HOME_BEACON_SEEN : 0;
    if (fresh) flags |= FLAG_GNSS_VALID;
    if (wantsFix && fix.valid && !fresh) flags |= FLAG_STALE_FIX;
    if (cellularFailure || (wantsFix && (gpsFailure || !fresh))) flags |= FLAG_ERROR_PRESENT;
    const bp_status_t status = profile == PROFILE_LOST ? STATUS_LOST
        : home ? STATUS_HOME : fresh ? STATUS_OUT_AND_ABOUT : STATUS_ERROR;
    pkt_init(bytes, source, destination, seq, utc, status, profile, flags, reason);
    if (fresh) pkt_set_gps(bytes, fix.latE7, fix.lonE7);
    pkt_set_quality(bytes, 0, fresh ? fix.accuracyM : 0,
                    wantsFix && age < 65535 ? uint16_t(age) : 65535);
    pkt_set_sat_count(bytes, fresh ? fix.satellites : 255);
    pkt_add_tlv_u32(bytes, TLV_UPTIME_S, uptime);
    const uint8_t length = pkt_finalize(bytes);
    bp_hmac_sha256_truncated8(key, 32, bytes, length - BP_AUTH_TAG_SIZE, bytes + length - BP_AUTH_TAG_SIZE);
    return length;
}

} // namespace walter
