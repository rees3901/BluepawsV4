#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "collar_report_policy.h"
int main() {
    for (auto profile : {PROFILE_NORMAL, PROFILE_POWERSAVE, PROFILE_ACTIVE, PROFILE_DEBUG, PROFILE_LOST}) {
        for (bool home : {false, true}) {
            const auto expected = profile == PROFILE_LOST ? STATUS_LOST : home ? STATUS_HOME : STATUS_OUT_AND_ABOUT;
            assert(collarLocationStatus(profile, home) == expected);
            const bool suppress = home && profile != PROFILE_LOST;
            for (bool valid : {false, true}) {
                for (bool previous : {false, true}) {
                    auto flags = collarReportFlags(profile, home, valid, previous, BP_ERROR_GPS);
                    assert(bool(flags & FLAG_GNSS_VALID) == valid);
                    assert(bool(flags & FLAG_HOME_BEACON_SEEN) == home);
                    assert(bool(flags & FLAG_STALE_FIX) == (!valid && previous && !suppress));
                    assert(bool(flags & FLAG_ERROR_PRESENT) == !suppress);
                    uint8_t packet[BP_MAX_PACKET_SIZE];
                    pkt_init(packet, 1001, 16, 1, 0, expected, profile, flags, TX_TELEMETRY);
                    const auto size = pkt_finalize(packet);
                    assert(pkt_validate_structure(packet, size));
                    assert(pkt_validate_crc(packet, size));
                    assert(pkt_status(packet) == expected);
                }
            }
            for (auto error : {BP_ERROR_RF, BP_ERROR_CELLULAR, BP_ERROR_MODULE}) {
                assert(collarReportFlags(profile, home, false, true, error) & FLAG_ERROR_PRESENT);
                assert(collarReportFlags(profile, home, false, true, error, true) & FLAG_ERROR_PRESENT);
            }
            auto presence = collarReportFlags(profile, home, false, true, BP_ERROR_GPS, true);
            assert(!(presence & (FLAG_GNSS_VALID | FLAG_STALE_FIX | FLAG_ERROR_PRESENT)));
        }
    }
    puts("PASS: Home/Out/Lost, fix validity, stale flags, independent faults, presence and wire CRC");
}
