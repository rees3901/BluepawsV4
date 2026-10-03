#pragma once
#include <stdint.h>

namespace personal_display {
enum class ButtonAction { None, Next, Home };
// Active-low USER/BOOT switch: stable edges, one action per press, no repeats.
class Button {
    bool raw_ = false, down_ = false, held_ = false;
    uint32_t changed_ = 0, pressed_ = 0;
public:
    ButtonAction tick(bool pressed, uint32_t now) {
        if (pressed != raw_) { raw_ = pressed; changed_ = now; }
        if (raw_ != down_ && uint32_t(now - changed_) >= 35) {
            down_ = raw_;
            if (down_) { pressed_ = now; held_ = false; }
            else if (!held_) return ButtonAction::Next;
        }
        if (down_ && !held_ && uint32_t(now - pressed_) >= 800) {
            held_ = true;
            return ButtonAction::Home;
        }
        return ButtonAction::None;
    }
};
}
