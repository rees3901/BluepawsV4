// Host-only scheduler tests: g++ -std=c++17 -Ihub/include tools/test_hub_reporting.cpp -o <test-output>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "hub_reporting.h"
int main() {
    HubReportingProfile profile=HubReportingProfile::PowerSave;
    assert(hubReportingIntervalMs(profile)==180000);
    for (const char* name : {"normal","power_save","active"}) {
        assert(parseHubReporting(name,profile));
        assert(!strcmp(name,hubReportingName(profile)));
        const uint32_t interval=hubReportingIntervalMs(profile);
        assert(interval==(!strcmp(name,"active") ? 30000u : !strcmp(name,"normal") ? 60000u : 180000u));
        assert(hubSelfReportDue(0,0,false,false,2000,profile)); // First report.
        assert(!hubSelfReportDue(interval-5001,0,true,false,2000,profile));
        assert(hubSelfReportDue(interval-5000,0,true,false,2000,profile));
        assert(!hubSelfReportDue(1999,0,true,true,2000,profile));
        assert(hubSelfReportDue(2000,0,true,true,2000,profile)); // Confirm without waiting for cadence.
        const uint32_t last=UINT32_MAX-100;
        assert(hubSelfReportDue(uint32_t(last+interval),last,true,false,2000,profile));
    }
    for (const char* invalid : {"lost_alert","debug","home","", "Active"}) {
        auto before=profile;
        assert(!parseHubReporting(invalid,profile)); assert(profile==before);
    }
    assert(!parseHubReporting(nullptr,profile));
    puts("PASS: hub-only profiles, independent confirmation cadence and clock rollover");
}
