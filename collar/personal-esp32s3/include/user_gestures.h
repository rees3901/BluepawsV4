#pragma once
#include <stdint.h>
namespace personal {
enum class Gesture : uint8_t { None, Single, Double, Long };
class UserGestures {
public:
    void begin(bool down, uint32_t now) {
        raw_ = stable_ = down; changed_ = pressed_ = now;
    }
    Gesture tick(bool down, uint32_t now) {
        if (down != raw_) { raw_ = down; changed_ = now; }
        if (raw_ != stable_ && now - changed_ >= 30) {
            stable_ = raw_;
            if (stable_) { pressed_ = now; held_ = false; }
            else if (!held_) {
                if (waiting_) { waiting_ = false; return Gesture::Double; }
                waiting_ = true; released_ = now;
            }
        }
        if (stable_ && !held_ && now - pressed_ >= 3000) {
            held_ = true; waiting_ = false; return Gesture::Long;
        }
        if (waiting_ && !stable_ && now - released_ >= 350) {
            waiting_ = false; return Gesture::Single;
        }
        return Gesture::None;
    }
    bool busy() const { return stable_ || raw_ || waiting_; }
private:
    bool raw_ = false, stable_ = false, held_ = false, waiting_ = false;
    uint32_t changed_ = 0, pressed_ = 0, released_ = 0;
};
}
