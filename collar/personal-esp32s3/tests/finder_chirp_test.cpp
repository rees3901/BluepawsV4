#include <assert.h>
#include "finder_chirp.h"
int main() {
    personal::FinderChirp f;
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
    f.start(100,0); assert(!f.active());
}
