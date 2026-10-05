#include <assert.h>
#include "user_gestures.h"
using personal::Gesture;
using personal::UserGestures;
int main() {
    UserGestures g;
    g.begin(false, 0);
    assert(g.tick(true, 10) == Gesture::None);
    assert(g.tick(false, 20) == Gesture::None); // Bounce is ignored.
    assert(g.tick(false, 60) == Gesture::None);
    assert(!g.busy());
    g.tick(true, 100); g.tick(true, 130);
    g.tick(false, 200); g.tick(false, 230);
    assert(g.tick(false, 579) == Gesture::None);
    assert(g.tick(false, 580) == Gesture::Single);
    assert(g.tick(false, 900) == Gesture::None);
    g.tick(true, 1000); g.tick(true, 1030);
    g.tick(false, 1100); g.tick(false, 1130);
    g.tick(true, 1200); g.tick(true, 1230);
    g.tick(false, 1300);
    assert(g.tick(false, 1330) == Gesture::Double);
    assert(g.tick(false, 1800) == Gesture::None);
    g.tick(true, 2000); g.tick(true, 2030);
    assert(g.tick(true, 5029) == Gesture::None);
    assert(g.tick(true, 5030) == Gesture::Long);
    assert(g.tick(true, 8000) == Gesture::None); // No repeated toggle.
    g.tick(false, 8010); g.tick(false, 8040);
    assert(g.tick(false, 8500) == Gesture::None); // No single after hold.
    // Sleep wake: the initiating press can be released before setup finishes.
    g = UserGestures(); g.begin(true, 100);
    g.tick(false, 100); g.tick(false, 130);
    assert(g.tick(false, 480) == Gesture::Single);
    // A second press held long overrides the pending single/double.
    g = UserGestures(); g.begin(true, 0);
    g.tick(false, 100); g.tick(false, 130);
    g.tick(true, 200); g.tick(true, 230);
    assert(g.tick(true, 3230) == Gesture::Long);
    // Hold duration is safe across millis rollover.
    g = UserGestures(); g.begin(true, 0xfffffff0u);
    assert(g.tick(true, uint32_t(0xfffffff0u+2999)) == Gesture::None);
    assert(g.tick(true, uint32_t(0xfffffff0u+3000)) == Gesture::Long);
}
