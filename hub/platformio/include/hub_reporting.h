#pragma once
#include <stdint.h>
#include <string.h>

// Hub self-report cadence only. Never changes RX, BLE, Wi-Fi or cloud-control tasks.
enum class HubReportingProfile : uint8_t { PowerSave, Normal, Active };
inline const char* hubReportingName(HubReportingProfile p) {
    switch (p) {
        case HubReportingProfile::Normal: return "normal";
        case HubReportingProfile::Active: return "active";
        default: return "power_save";
    }
}
inline bool parseHubReporting(const char* name, HubReportingProfile &out) {
    if (!name) return false;
    if (!strcmp(name,"power_save")) out=HubReportingProfile::PowerSave;
    else if (!strcmp(name,"normal")) out=HubReportingProfile::Normal;
    else if (!strcmp(name,"active")) out=HubReportingProfile::Active;
    else return false;
    return true;
}
inline uint32_t hubReportingIntervalMs(HubReportingProfile p) {
    switch (p) {
        case HubReportingProfile::Normal: return 60000;
        case HubReportingProfile::Active: return 30000;
        default: return 180000;
    }
}
inline bool hubSelfReportDue(uint32_t now, uint32_t last, bool posted,
                             bool requested, uint32_t retryMs, HubReportingProfile profile) {
    // The cloud worker wakes at most five seconds later; unsigned subtraction
    // keeps the schedule valid across millis() wraparound.
    return !posted || uint32_t(now-last) >= (requested ? retryMs : hubReportingIntervalMs(profile)-5000u);
}
