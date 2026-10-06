#include <assert.h>
#include "finder_flash.h"
int main() {
    personal::FinderFlash f;
    assert(!f.tick(0));
    f.start(100);
    assert(f.tick(100)); assert(f.tick(249)); assert(!f.tick(250));
    assert(!f.tick(329)); assert(f.tick(330)); assert(!f.tick(480));
    assert(f.tick(560)); assert(f.tick(709)); assert(!f.tick(710));
    assert(!f.active());
    f.start(0xfffffff0u);
    assert(f.tick(0x20u)); assert(!f.tick(uint32_t(0xfffffff0u+610u)));
    f.start(10); f.stop(); assert(!f.tick(11));
    f.start(100,1); assert(f.tick(249)); assert(!f.tick(250)); assert(!f.active());
    f.start(100,4);
    for (unsigned i=0;i<4;++i) {
        assert(f.tick(100+i*200)); assert(f.tick(169+i*200));
        if (i<3) { assert(!f.tick(170+i*200)); assert(!f.tick(299+i*200)); }
    }
    assert(!f.tick(770)); assert(!f.active());
    f.start(100,7);
    for (unsigned i=0;i<7;++i) { assert(f.tick(100+i*140)); assert(f.tick(169+i*140)); if(i<6) assert(!f.tick(170+i*140)); }
    assert(!f.tick(1010)); assert(!f.active());
    f.start(100,8); assert(!f.active());
    f.start(100,0); assert(!f.active());
}
