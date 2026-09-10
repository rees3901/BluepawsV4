#ifndef BLUEPAWS_OFFLINE_ACCESS_H
#define BLUEPAWS_OFFLINE_ACCESS_H

#include <Arduino.h>
#include <IPAddress.h>

static constexpr uint8_t BP_LOCAL_SESSION_LIMIT = 8;

class OfflineAccess {
public:
    bool setPin(const String &pin);
    void disable();
    bool enabled() const { return pin_.length() == 4; }
    bool unlock(const String &pin, const IPAddress &address, String &tokenOut,
                uint32_t &retryAfterSeconds);
    bool authorize(const String &token, const IPAddress &address) const;
    uint8_t sessionCount() const;

private:
    struct Session {
        bool active = false;
        IPAddress address;
        String token;
    };

    String pin_;
    Session sessions_[BP_LOCAL_SESSION_LIMIT];
    uint8_t failedAttempts_ = 0;
    uint32_t blockedUntilMs_ = 0;

    static bool validPin(const String &pin);
    static String randomToken();
};

#endif
