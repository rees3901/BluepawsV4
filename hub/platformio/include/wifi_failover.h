#pragma once
#include <stdint.h>

// Pure policy: the network task alone performs the returned radio operations.
// One outage has one deadline, not a fresh timeout for every connection attempt.
class WifiFailover {
public:
    enum class Phase { Idle, Primary, Secondary, OnlinePrimary, OnlineSecondary, OffGrid };
    enum class Action { None, ConnectPrimary, ConnectSecondary, ConnectedPrimary,
                        ConnectedSecondary, StartOffGrid };
    static constexpr uint32_t RecoveryMs = 30000;

    WifiFailover(bool primary, bool secondary) : primary_(primary), secondary_(secondary) {}
    Action begin(uint32_t now, bool preferSecondary = false) {
        started_ = now;
        if (!primary_ && !secondary_) return offGrid();
        phase_ = (secondary_ && (preferSecondary || !primary_)) ? Phase::Secondary : Phase::Primary;
        switched_ = false;
        return connectAction();
    }
    Action offGrid() { phase_ = Phase::OffGrid; return Action::StartOffGrid; }
    Action tick(uint32_t now, bool connected) {
        if (phase_ == Phase::OffGrid || phase_ == Phase::Idle) return Action::None;
        if (phase_ == Phase::OnlinePrimary || phase_ == Phase::OnlineSecondary) {
            return connected ? Action::None : begin(now);
        }
        if (connected) {
            bool primary = phase_ == Phase::Primary;
            phase_ = primary ? Phase::OnlinePrimary : Phase::OnlineSecondary;
            return primary ? Action::ConnectedPrimary : Action::ConnectedSecondary;
        }
        const uint32_t elapsed = now - started_; // millis() rollover safe
        if (elapsed >= RecoveryMs) return offGrid();
        if (primary_ && secondary_ && !switched_ && elapsed >= RecoveryMs / 2) {
            switched_ = true;
            phase_ = phase_ == Phase::Primary ? Phase::Secondary : Phase::Primary;
            return connectAction();
        }
        return Action::None;
    }
    uint32_t remaining(uint32_t now) const {
        if (phase_ != Phase::Primary && phase_ != Phase::Secondary) return 0;
        uint32_t elapsed = now - started_;
        return elapsed >= RecoveryMs ? 0 : RecoveryMs - elapsed;
    }
    Phase phase() const { return phase_; }
    static const char *name(Phase phase) {
        switch (phase) {
        case Phase::Primary: return "searching_primary";
        case Phase::Secondary: return "searching_secondary";
        case Phase::OnlinePrimary: return "online_primary";
        case Phase::OnlineSecondary: return "online_secondary";
        case Phase::OffGrid: return "off_grid";
        default: return "starting";
        }
    }
private:
    Action connectAction() const {
        return phase_ == Phase::Primary ? Action::ConnectPrimary : Action::ConnectSecondary;
    }
    bool primary_, secondary_, switched_ = false;
    uint32_t started_ = 0;
    Phase phase_ = Phase::Idle;
};
