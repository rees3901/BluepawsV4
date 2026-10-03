#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace personal {
// Entire attempt, including stabilization, has one hard deadline. A quality
// drop resets stabilization but never restarts the acquisition deadline.
class GnssAttempt {
    uint32_t started_, stable_ = 0;
    bool stabilizing_ = false;
public:
    static constexpr uint32_t LimitMs = 60000, StableMs = 10000;
    explicit GnssAttempt(uint32_t now) : started_(now) {}
    bool expired(uint32_t now) const { return uint32_t(now-started_) >= LimitMs; }
    bool accept(bool good, uint32_t now) {
        if (expired(now)) return false;
        if (!good) { stabilizing_ = false; return false; }
        if (!stabilizing_) { stable_ = now; stabilizing_ = true; }
        return uint32_t(now-stable_) >= StableMs;
    }
};

// Decode checksum-valid diagnostic fields only; never print coordinates.
// GSV is per talker/message, not a sum across constellations.
struct NmeaDiagnostics {
    char line[128] = {}, talker[3] = {}, txt[80] = {};
    uint8_t used = 0;
    uint32_t valid = 0, bad = 0, gga = 0, gsa = 0, gsv = 0, rmc = 0, texts = 0;
    int quality = -1, fixType = -1, visible = -1, maxSnr = -1;
    char rmcStatus = '?';
    bool txtReady = false;
    void feed(char c) {
        if (c == '$') { used = 0; line[used++] = c; return; }
        if (!used || c == '\r') return;
        if (c != '\n') {
            if (used < sizeof(line)-1) line[used++] = c;
            else { used = 0; ++bad; }
            return;
        }
        line[used] = 0; used = 0;
        char* star = strchr(line, '*');
        if (!star || strlen(star+1)!=2) { ++bad; return; }
        char* end; const long expected = strtol(star+1,&end,16);
        uint8_t checksum = 0;
        for (char* p=line+1; p<star; ++p) checksum ^= uint8_t(*p);
        if (*end || expected!=checksum) { ++bad; return; }
        *star = 0;
        char* fields[32] = {line+1}; size_t count = 1;
        for (char* p=line+1; *p; ++p) if (*p==',') {
            *p = 0; if (count<32) fields[count++] = p+1;
        }
        ++valid;
        if (strlen(fields[0])!=5) return;
        talker[0]=fields[0][0]; talker[1]=fields[0][1]; talker[2]=0;
        const char* type=fields[0]+2;
        if (!strcmp(type,"GGA") && count>8) { ++gga; quality=*fields[6]?atoi(fields[6]):-1; }
        else if (!strcmp(type,"GSA") && count>2) { ++gsa; fixType=*fields[2]?atoi(fields[2]):-1; }
        else if (!strcmp(type,"RMC") && count>2) { ++rmc; rmcStatus=*fields[2]?*fields[2]:'?'; }
        else if (!strcmp(type,"GSV") && count>3) {
            ++gsv; visible=*fields[3]?atoi(fields[3]):-1;
            for (size_t i=7;i<count;i+=4) if (*fields[i]) {
                const int snr=atoi(fields[i]); if (snr>maxSnr) maxSnr=snr;
            }
        } else if (!strcmp(type,"TXT") && count>4) {
            ++texts;
            if (texts<=4) { strncpy(txt,fields[4],sizeof(txt)-1); txtReady=true; }
        }
    }
};
}
