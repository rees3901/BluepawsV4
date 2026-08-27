#include "wifi_failover.h"
#include <cassert>
#include <cstdio>
using A = WifiFailover::Action;
using P = WifiFailover::Phase;
int main() {
    WifiFailover both(true, true);
    assert(both.begin(100) == A::ConnectPrimary);
    assert(both.tick(15099, false) == A::None);
    assert(both.tick(15100, false) == A::ConnectSecondary);
    assert(both.remaining(20100) == 10000);
    assert(both.tick(30099, false) == A::None);
    assert(both.tick(30100, false) == A::StartOffGrid);
    assert(both.tick(90000, true) == A::None); // restored Wi-Fi cannot eject AP users
    assert(both.phase() == P::OffGrid);
    assert(both.begin(100000, true) == A::ConnectSecondary); // explicit confirmed retry
    assert(both.tick(100001, true) == A::ConnectedSecondary);
    assert(both.tick(100002, true) == A::None);
    assert(both.tick(100003, false) == A::ConnectPrimary); // new outage, fresh total budget
    assert(both.tick(100004, true) == A::ConnectedPrimary);
    WifiFailover primary(true, false);
    assert(primary.begin(0) == A::ConnectPrimary);
    assert(primary.tick(15000, false) == A::None);
    assert(primary.tick(29999, true) == A::ConnectedPrimary);
    assert(primary.tick(40000, false) == A::ConnectPrimary);
    assert(primary.tick(70000, false) == A::StartOffGrid);
    WifiFailover secondary(false, true);
    assert(secondary.begin(0) == A::ConnectSecondary);
    assert(secondary.tick(30000, false) == A::StartOffGrid);
    WifiFailover none(false, false);
    assert(none.begin(0) == A::StartOffGrid);
    assert(none.tick(30000, true) == A::None);
    WifiFailover wrap(true, true);
    constexpr uint32_t start = UINT32_MAX - 10000;
    assert(wrap.begin(start) == A::ConnectPrimary);
    assert(wrap.tick(start + 15000U, false) == A::ConnectSecondary);
    assert(wrap.tick(start + 30000U, false) == A::StartOffGrid);
    WifiFailover prefer(true, true);
    assert(prefer.begin(0, true) == A::ConnectSecondary);
    assert(prefer.tick(15000, false) == A::ConnectPrimary);
    assert(prefer.tick(30000, false) == A::StartOffGrid);
    std::puts("Wi-Fi failover policy: all tests passed");
}
