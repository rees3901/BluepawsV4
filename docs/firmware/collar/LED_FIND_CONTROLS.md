# LED Find controls

The dashboard hides the former buzzer controls without removing their source.
The LED dialog offers Flash now, Start repeating, and Stop LED cycle. Each cycle
is seven 70ms flashes with 70ms gaps (about one second). Repeat defaults to once
per minute for ten minutes. Intervals are 10/30 seconds and 1/2/5/10 minutes;
durations are 10/30 seconds, 1/5/10/15/30 minutes, and 1/2/4 hours.

This is a reusable V4 command, initially gated to the fitted personal batch
3001–3004 in the UI and guarded queue. Other devices remain disabled until
compatible firmware/capability discovery is available. Only 3004 has received
the new collar image so far. Do not infer firmware support from the device ID.

## Delivery and cancellation

`bluepaws_queue_device_command` accepts `led_find` with one of:

```json
{"action":"flash"}
{"action":"repeat","duration_s":600,"interval_s":60}
{"action":"stop"}
```

Repeat accepts integral durations 10–14400 seconds and intervals 10–600 seconds.
The existing Family owner/member permissions apply. A new LED request supersedes
pending/sent LED requests. Invalid requests cannot cancel existing commands.
The browser explicitly requests ten-minute command expiry. Delivery waits for a
check-in; Flash now and Stop do not wake an unreachable or sleeping collar.
Normal/Power Save can therefore miss the expiry window. Use Active/Emergency Lost
when searching and check the actual ACK rather than assuming immediate execution.

The repeat duration starts on collar receipt, independent of browser lifetime.
The collar owns the timeout, retains it through deep sleep, and persists its UTC
deadline through resets. Duplicate/stale LED sequences do not restart schedules.
Stop cancels a repeat and suppresses automatic Lost flashes until Lost is entered
again. Flash now is a single cycle and leaves an already running repeat intact.

The compatible personal firmware flashes once per minute while in Emergency Lost.
It can wake for LED-only cycles without requesting GPS or altering the regular
telemetry deadline. Button gestures retain their separate behaviour.

## LoRa extension

The hub uses existing `TX_CONFIG` with no `TLV_PROFILE`, and these assigned
extension tags. Header, packet length, ACK identity and profile commands are
unchanged; the shared protocol source is not modified by this implementation.

| Tag | Encoding | Value |
| --- | --- | --- |
| `0xFA` LED action | u8 | 0=flash, 1=repeat, 2=stop |
| `0xFB` LED duration | little-endian u16 | repeat seconds; 0 for flash/stop |
| `0xFC` LED interval | little-endian u16 | repeat interval 10–600 seconds; 60 for flash/stop |

Older collar firmware rejects CONFIG without PROFILE and does not ACK it.
The compatible collar validates all three fields and rejects mixed LED/profile
commands and unfitted outputs. The existing downlink security limitations remain
those documented in `COLLAR_DOWNLINK_COMMANDS.md`.

## Release and rollback

The public feature branch is based on updated main and contains web, queue
migration, hub relay and documentation only. Personal collar/T190 adapters stay
on `codex/personal-legacy-collars`; do not merge that branch.

The variable-interval firmware extends the earlier minute-only image. Existing
saved LED schedules with the previous structure size are discarded on cold boot;
send a fresh repeat command after updating. Devices need updated collar and hub
firmware before using intervals other than 60 seconds or durations above one hour.

Release order: compatible collar/hub firmware, reviewed queue migration, then
web release. No Edge Function change is needed: the existing envelope forwards
command type and payload. Before relying on it, verify GUI request → hub relay →
collar ACK and visible flash; test repeat expiry, Stop, sleep, reset and radio loss.

Rollback the web commit to hide the controls. Cancel pending LED commands before
restoring previous firmware. Keep the additive queue migration while LED rows
exist; dropping the new type constraint allowance would fail on those rows.
Previously saved firmware remains in the private recovery bundle.

## Verification to date

- Web build/typecheck and unit tests passed; lint has no errors (existing vendor
  MapLibre warnings). Desktop/mobile preview buttons and duration selector work;
  browser reported no runtime errors. Preview used mock callbacks, not live writes.
- Isolated PostgreSQL tests passed bounds, malformed inputs, Family permissions,
  batch gating, supersession and existing queue permission regressions.
- Canonical hub, personal T190 and collar builds passed. Native collar tests cover
  minute cadence, expiry, Stop/Lost override, sequence rollover/stale rejection,
  addressing, missing clock, unsupported output and duplicate packets.
- Collar 3004 was flashed on COM23 with hashes verified. Live backend migration,
  production web release, hub flash and real GUI-to-LED validation remain pending.
