#include <cassert>
#include <cstdio>
#include <cstring>
#include "gnss_recovery.h"
void sentence(personal::NmeaDiagnostics& d,const char* body) {
    unsigned char crc=0; for (auto* p=body;*p;++p) crc^=*p;
    char line[200]; snprintf(line,sizeof(line),"$%s*%02X\r\n",body,crc);
    for (auto* p=line;*p;++p) d.feed(*p);
}
int main() {
    personal::GnssAttempt a(100);
    assert(!a.accept(false,15100)); // recovery continues beyond old warm timeout
    assert(!a.accept(true,20100));
    assert(!a.accept(false,29100)); // quality drop discards stabilization
    assert(!a.accept(true,35100));
    assert(!a.accept(true,45099));
    assert(a.accept(true,45100));
    personal::GnssAttempt late(0);
    assert(!late.accept(true,55000));
    assert(late.expired(60000));
    assert(!late.accept(true,65000)); // stabilization cannot extend hard deadline
    personal::GnssAttempt wrap(0xFFFFFF00U);
    assert(!wrap.accept(true,0xFFFFFF00U));
    assert(wrap.accept(true,0xFFFFFF00U+10000U));
    assert(wrap.expired(0xFFFFFF00U+60000U));
    personal::NmeaDiagnostics d;
    sentence(d,"GNGGA,120000,5100.00,N,00200.00,W,1,07,1.2,10,M,0,M,,");
    assert(d.gga==1 && d.quality==1);
    sentence(d,"GNGSA,A,3,,,,,,,,,,,,,1.2,1.0,1.0");
    assert(d.gsa==1 && d.fixType==3);
    sentence(d,"GPRMC,120000,V,,,,,,,010126,,");
    assert(d.rmcStatus=='V');
    sentence(d,"GPGSV,1,1,02,01,30,100,25,02,40,110,35");
    assert(d.visible==2 && d.maxSnr==35);
    sentence(d,"GPTXT,01,01,02,receiver startup");
    assert(d.txtReady && !strcmp(d.txt,"receiver startup"));
    const char* broken="$GNGGA,0*ZZ\n";
    for (auto* p=broken;*p;++p) d.feed(*p);
    assert(d.bad==1 && d.gga==1);
    sentence(d,"GNGGA,,,,,,0,,,0,M,0,M,,");
    assert(d.quality==0); // empty fields retain their indexes
    puts("PASS: recovery, quality loss, strict deadline, rollover, checksum and NMEA diagnostics");
}
