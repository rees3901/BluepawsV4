#include <cassert>
#include <cstdio>
#include "display_button.h"
using personal_display::Button;
using personal_display::ButtonAction;
int main() {
    Button b;
    assert(b.tick(false,0)==ButtonAction::None);
    // Contact bounce must not turn one press into multiple page changes.
    b.tick(true,10); b.tick(false,20); b.tick(true,30);
    assert(b.tick(true,65)==ButtonAction::None);
    b.tick(false,100); b.tick(true,110); b.tick(false,120);
    assert(b.tick(false,155)==ButtonAction::Next);
    assert(b.tick(false,900)==ButtonAction::None);
    b.tick(true,1000); b.tick(true,1035);
    assert(b.tick(true,1834)==ButtonAction::None);
    assert(b.tick(true,1835)==ButtonAction::Home);
    assert(b.tick(true,3000)==ButtonAction::None);
    b.tick(false,3010);
    assert(b.tick(false,3045)==ButtonAction::None); // no next after hold
    // Debounce and hold timing must work across millis() rollover.
    Button rollover;
    rollover.tick(true,0xFFFFFFF0U);
    rollover.tick(true,0x13U);
    assert(rollover.tick(true,0x333U)==ButtonAction::Home);
    puts("PASS: bounce, short press, hold, no repeat and timer rollover");
}
