#pragma once
#include <bp_protocol.h>

// Location state and GNSS health are independent. No board or driver dependencies.
inline bp_status_t collarLocationStatus(bp_profile_t profile, bool homeSeen) {
    return profile == PROFILE_LOST ? STATUS_LOST : homeSeen ? STATUS_HOME : STATUS_OUT_AND_ABOUT;
}

inline uint8_t collarReportFlags(bp_profile_t profile, bool homeSeen,
                                bool validFix, bool havePreviousFix,
                                bp_error_t error, bool presenceOnly = false) {
    const bool atHome = homeSeen && profile != PROFILE_LOST;
    uint8_t flags = homeSeen ? FLAG_HOME_BEACON_SEEN : 0;
    if (!presenceOnly) {
        if (validFix) flags |= FLAG_GNSS_VALID | FLAG_FIX_3D;
        else if (havePreviousFix && !atHome) flags |= FLAG_STALE_FIX;
    }
    // Indoor GNSS failure is expected at Home. Keep RF/modem/module faults.
    if (error != BP_ERROR_NONE && (error != BP_ERROR_GPS || (!atHome && !presenceOnly)))
        flags |= FLAG_ERROR_PRESENT;
    return flags;
}
