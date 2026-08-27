#include "offline_access.h"

#include <esp_system.h>

bool OfflineAccess::validPin(const String &pin) {
    if (pin.length() != 4) return false;
    for (size_t i = 0; i < pin.length(); ++i) {
        if (pin[i] < '0' || pin[i] > '9') return false;
    }
    return true;
}

String OfflineAccess::randomToken() {
    char value[33];
    snprintf(value, sizeof(value), "%08lx%08lx%08lx%08lx",
             (unsigned long)esp_random(), (unsigned long)esp_random(),
             (unsigned long)esp_random(), (unsigned long)esp_random());
    return String(value);
}

bool OfflineAccess::setPin(const String &pin) {
    if (!validPin(pin)) return false;
    disable();
    pin_ = pin;
    return true;
}

void OfflineAccess::disable() {
    pin_ = "";
    failedAttempts_ = 0;
    blockedUntilMs_ = 0;
    for (uint8_t i = 0; i < BP_LOCAL_SESSION_LIMIT; ++i) {
        sessions_[i].active = false;
        sessions_[i].token = "";
    }
}

bool OfflineAccess::unlock(const String &pin, const IPAddress &address,
                           String &tokenOut, uint32_t &retryAfterSeconds) {
    retryAfterSeconds = 0;
    if (!enabled()) return false;
    uint32_t now = millis();
    if (blockedUntilMs_ != 0 && (int32_t)(blockedUntilMs_ - now) > 0) {
        retryAfterSeconds = (blockedUntilMs_ - now + 999) / 1000;
        return false;
    }
    if (pin != pin_) {
        failedAttempts_++;
        if (failedAttempts_ >= 5) {
            blockedUntilMs_ = now + 60000UL;
            failedAttempts_ = 0;
            retryAfterSeconds = 60;
        }
        return false;
    }

    failedAttempts_ = 0;
    blockedUntilMs_ = 0;
    for (uint8_t i = 0; i < BP_LOCAL_SESSION_LIMIT; ++i) {
        if (!sessions_[i].active || sessions_[i].address == address) {
            sessions_[i].active = true;
            sessions_[i].address = address;
            sessions_[i].token = randomToken();
            tokenOut = sessions_[i].token;
            return true;
        }
    }
    return false;
}

bool OfflineAccess::authorize(const String &token, const IPAddress &address) const {
    if (!enabled()) return true;
    if (token.length() == 0) return false;
    for (uint8_t i = 0; i < BP_LOCAL_SESSION_LIMIT; ++i) {
        if (sessions_[i].active && sessions_[i].address == address
            && sessions_[i].token == token) return true;
    }
    return false;
}

uint8_t OfflineAccess::sessionCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < BP_LOCAL_SESSION_LIMIT; ++i) {
        if (sessions_[i].active) count++;
    }
    return count;
}
