# GM02SP GNSS time foundation

Status: **foundation implemented; not connected to the live modem task and not hardware-verified**.
The GM02SP is not yet fitted. No firmware flashing, UART probing, power changes,
LTE registration or changes to the running testbed were part of this work.

## Commands and meaning

Read the GNSS subsystem's current clock over its AT UART:

```text
AT+LPGNSSUTCTIME?
+LPGNSSUTCTIME: "2026-08-27T09:00:00"
OK
```

The response above is an illustrative fixture. An uninitialised clock can return
`"NO_CLOCK_DEFINED"`. A successful `OK` alone does not contain usable time.

For a new positioning attempt, the documented command is
`AT+LPGNSSFIXPROG="single"`. The asynchronous `+LPGNSSFIXREADY` report includes
`fix_id`, a quoted ISO UTC timestamp, time-to-fix and the position/quality fields.
`AT+LPGNSSGETFIX[=<fix_id>]` retrieves stored fixes per our Sequans command notes;
a stored fix timestamp is **not** necessarily the current time. This foundation
extracts the timestamp from FIXREADY only; GETFIX/full fix decoding is deferred.

The **setter** `AT+LPGNSSUTCTIME="..."` provides a time hint to assist acquisition;
it does not fetch satellite time. Consequently the clock readback is labelled
`ModemGnssClock`, not satellite-verified. A parsed FIXREADY time is labelled
`FixTimestamp`; the integration must still validate the full result and correlate
it with the current acquisition before using it to synchronise the collar.

`AT+CCLK?` reads the modem RTC, potentially updated by cellular network time. It
is a separate fallback, not implemented here, and must not be labelled GNSS time.
An indoor cold start may need a trusted RTC/network time hint before GNSS succeeds;
do not assume satellites will always supply a usable clock at boot.

## Code boundary

- `collar/include/gm02sp_time.h`, `collar/src/gm02sp_time.cpp`: standalone C++11,
  compiled by the collar environment, with no Arduino, heap, UART or RTOS dependency.
- `ClockQuery`: nonblocking transaction for the read command. It waits for the
  expected timestamp **and** final OK, rejects malformed/duplicate responses,
  distinguishes no-clock/modem-error/timeout, and returns unrelated URCs to the
  dispatcher. Default deadline is three seconds, checked with wrap-safe ticks.
- `parseUtc`: validates calendar dates and UTC format, truncates fractional
  seconds, rejects local-time offsets, uninitialised epochs and uint32 overflow.
  It does not use `mktime`, local timezone, compiler build dates or persisted fixes.
  The 2023 lower bound is a plausibility floor, not proof that a date is current.
- `TimeAnchor`: optional RAM holdover clock using an externally supplied 64-bit
  monotonic millisecond count. Reads include a caller-chosen maximum age; expired,
  reset or overflowed clocks fail without replacing the caller's value. A valid
  sample advances during sleep without requiring a modem wake just for time.

## Future single-owner FreeRTOS modem integration

1. The modem task owns UART reads/writes and sequences GNSS versus LTE. Do not add
   a second reader alongside `cellularSendAT`; that would steal responses/URCs.
2. Once the modem is already awake and ready, call `query.begin(monotonic32Ms)`;
   only when true, send `kReadGnssClock` (includes the CR terminator).
3. The UART dispatcher frames complete CR/LF-stripped lines, enforcing its buffer
   limit and rejecting overlong/truncated lines. Pass lines to `query.onLine`;
   false means the usual URC handler must still receive them. Call `query.poll`
   regularly even if no bytes arrive. Do not use substring matching for `OK`.
4. After timeout or framing failure, drain/resynchronise the AT transaction before
   retrying, so a late response cannot be assigned to a new command. No tight retries.
5. On `query.sample(sample)` success, apply source/freshness policy before anchoring.
   For satellite-derived synchronisation, prefer a fully validated current fix;
   merely parsing the timestamp must **not** set `GNSS_VALID`, HOME or other flags.
6. Publish the accepted clock under the existing task/mutex policy. Record its
   source and age. Packet construction should use current UTC from the anchor;
   the last position's timestamp remains separate for fix-age/persistence purposes.
7. Supply an RTC/tick extension that continues through system-on sleep. Raw
   `millis()` wraps and cannot simply be cast to 64 bits. Clear the anchor after
   reset/deep power loss unless a separately validated retained RTC is available.

Do not power the modem up solely because a packet builder asks for a timestamp.
GNSS and active LTE RF are non-concurrent on this module: retain the collar's
existing ownership/power sequencing design, then verify it on the real hardware.

## Deliberately unchanged for now

The live `main.cpp` GNSS path still contains legacy `AT+SQNGNSS*` placeholders;
they are **not verified GM02SP commands**. Do not enable that path on production
hardware. Replace it with the documented LPGNSS sequence, validated position
parsing and the single-owner UART task when the module arrives. This new reader
is not silently inserted into those placeholders.

The testbed still uses synthetic coordinates/build-derived time. This foundation
does not repair its clock or claim that its dates are satellite-derived. Persisted
last-fix time is not a clock synchronisation source after reboot. TLV v1.2,
Supabase, the hub and the simulator remain unchanged.

## Verification

From the repository root with MinGW g++ and PlatformIO installed:

```powershell
New-Item -ItemType Directory -Force .pio/native-tests | Out-Null
g++ -std=c++11 -Wall -Wextra -Werror -Icollar/include collar/src/gm02sp_time.cpp collar/tests/gm02sp_time_test.cpp -o .pio/native-tests/gm02sp_time_test.exe
if ($LASTEXITCODE -ne 0) { throw "Native build failed" }
& ./.pio/native-tests/gm02sp_time_test.exe
if ($LASTEXITCODE -ne 0) { throw "Native tests failed" }
& "$env:USERPROFILE/.platformio/penv/Scripts/python.exe" -m platformio run -e collar
```

Tests cover leap years, invalid dates, UTC independence, fractional seconds,
2038/2106-safe arithmetic, no-clock, errors, missing final OK, timeout, late data,
duplicate replies, interleaved URCs, tick rollover and holdover expiry/reset.
Fixtures are synthetic. Hardware acceptance still requires UART capture of the
actual modem firmware, cold/no-clock boot, fresh fix, indoor no-fix, LTE/GNSS
handover and sleep/wake clock continuity. Compare against an independent UTC clock.

## Primary references (checked 2026-08-27)

- [QuickSpot GM02SP GNSS command implementation](https://github.com/QuickSpot/walter-arduino/blob/main/src/proto/WalterGNSS.cpp):
  read/set UTC commands and single-fix action.
- [QuickSpot GM02SP response handling](https://github.com/QuickSpot/walter-arduino/blob/main/src/WalterModem.cpp):
  LPGNSSUTCTIME no-clock response and FIXREADY timestamp field.
- [QuickSpot GNSS reference](https://github.com/QuickSpot/walter-documentation/blob/main/walter-modem/arduino_esp-idf/reference/gnss.md).
- [Sequans GNSS/LTE concurrency guidance](https://forum.sequans.com/t/can-i-use-gnss-positioning-while-maintaining-lte-m-or-nb-iot-connectivity-simultaneously/209).
- `docs/USEFUL_GM02SP_COMMANDS.md`: project's Sequans manual-derived reference.

The implementation here is original Bluepaws code, not a copied Walter driver;
no Walter-only library or ESP32 pin assignments are added to the nRF52840 build.
