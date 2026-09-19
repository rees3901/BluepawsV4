# BluePaws Power Profile Standard

This document is the human-readable standard for BluePaws power profiles.
Implementation work should check this document first, then verify the compiled
values in `shared/lib/BluepawsProtocol/bp_config.h`.

Power profiles describe collar runtime behaviour: wake cadence, radio power,
home check-ins, GNSS refresh, LTE use, and fallback behaviour. They are not the
same as Home Hub reporting profiles.

## Source of truth

The compiled profile table lives in:

- `shared/lib/BluepawsProtocol/bp_config.h`

Related behaviour and protocol documents:

- `docs/firmware/collar/COLLAR_RUNTIME_DECISIONS.md`
- `docs/protocol/TLV_PROTOCOL_V1_2.md`
- `docs/protocol/COLLAR_DOWNLINK_COMMANDS.md`
- `docs/firmware/home-hub/HUB_SELF_PRESENCE.md`

When changing profile behaviour, update this document, `bp_config.h`, and any
affected protocol or UI references in the same change.

## Collar profile summary

| Profile | Code | Customer visible | Intended use | TX power | Wake interval | LTE cadence |
|---|---:|---|---|---:|---:|---:|
| `POWER_SAVE` | 0 | Yes | Manual battery saving, low battery, or future mostly-home automation | 10 dBm | 30 min | Every 30 cycles while away; 3 hour heartbeat while home |
| `NORMAL` | 1 | Yes | Default everyday collar behaviour | 14 dBm | 10 min | Every 10 cycles while away; 1 hour heartbeat while home |
| `ACTIVE` | 2 | Yes | Higher-frequency monitoring, not an emergency state | 20 dBm | 60 sec | Every 5 cycles while away; 10 min heartbeat while home |
| `LOST_ALERT` | 3 | Yes, as emergency mode | Temporary emergency search mode | 20 dBm | No normal sleep cadence; 30 sec emergency cycle | Frequent/redundant; 60 sec heartbeat target |
| `DEBUG` | 4 | No | Development-only bench and pipeline testing | 14 dBm | 30 sec | Every cycle; 30 sec heartbeat |

Codes 5-15 are reserved.

## Collar detailed behaviour

| Profile | Home LoRa check-in | Home GNSS sanity refresh | Failed LoRa cycles before LTE | LED flashes | LED beacon | Continuous GNSS |
|---|---:|---:|---:|---:|---|---|
| `POWER_SAVE` | Every 2 BLE-home wakes | Every 10 BLE-home wakes | 3 | 3 | No | No |
| `NORMAL` | Every BLE-home wake | Every 10 BLE-home wakes | 3 | 5 | No | No |
| `ACTIVE` | Every BLE-home wake | Every 10 BLE-home wakes | 2 | 5 | No | No |
| `LOST_ALERT` | Emergency path, not normal home cadence | Every cycle where practical | 1 | 10 | Yes | Yes |
| `DEBUG` | Every BLE-home wake | Every BLE-home wake | 1 | 2 | No | No |

## Behaviour rules

- `NORMAL` is the fallback profile when a requested profile is unknown or invalid.
- `DEBUG` is a real TLV profile code but must remain development-only. It must not
  be exposed as a normal customer control.
- `LOST_ALERT` is deliberately expensive and must be temporary. It should be
  user-triggered during an active search, then auto-revert after the safety timeout.
- The Lost Mode maximum duration is 2 hours.
- The Lost Mode fallback profile is `ACTIVE`.
- The Lost Mode emergency cycle interval is 30 seconds.
- Wake check-ins are valid presence reports. They may omit GNSS and should update
  last-seen/presence without replacing the last known valid position.
- Home GNSS refresh is a sanity refresh, not the primary home presence mechanism.
- LTE heartbeats while home are time-based, not only cycle-count based.
- Failed LoRa cycles before LTE means consecutive unacknowledged LoRa report
  cycles before escalating to cellular fallback.

## Downlink command names

Backend command payloads use these profile names:

| Profile | Command payload value |
|---|---|
| `POWER_SAVE` | `power_save` |
| `NORMAL` | `normal` |
| `ACTIVE` | `active` |
| `LOST_ALERT` | `lost_alert` |
| `DEBUG` | `debug` |

The `set_profile` command can carry `debug`, but production UI must not expose
that option to customers.

## Home Hub reporting profiles

Home Hub reporting profiles are separate from collar power profiles. They change
how often the hub reports its own presence to the cloud. They do not slow LoRa
RX, BLE, the captive portal, local status, GNSS reads, or settings checks.

| Hub reporting profile | Cloud self-report interval | Cloud contact overdue |
|---|---:|---:|
| Power Save | 180 sec | 210 sec |
| Normal | 60 sec | 90 sec |
| Active | 30 sec | 60 sec |

There is no hub `LOST_ALERT` or `DEBUG` reporting profile.

## Implementation checklist

Before implementing any profile-sensitive feature, check:

- Does it use the profile enum values from TLV v1.2?
- Does it use the compiled timings and ratios from `bp_config.h`?
- Does it distinguish collar power profiles from hub reporting profiles?
- Does the UI avoid showing `DEBUG` to customers?
- Does Lost Alert have a clear exit path and timeout?
- Does no-GNSS wake-check-in behaviour preserve the last valid position?
- Does any profile change update documentation and tests in the same change?
