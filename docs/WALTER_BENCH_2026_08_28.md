# Walter commissioning — 2026-08-28

## Scope

Independent device **1010**, named **Walter LTE Testbed**, connected on **COM26 at
115200 baud**, with the user's 1NCE SIM and antenna confirmation. Existing WisMesh
COM23, hub COM7 and sniffer COM11 were not opened, flashed or reconfigured.

## Verified

| Check | Evidence/result |
| --- | --- |
| Board identification | ESP32-S3 revision0.2, 16 MB quad flash, 2 MB embedded PSRAM |
| Factory rollback copy | Full 16,777,216-byte read before flashing; SHA-256 `3876cabfe983a7da2e05b245a61e446e6595e2c07ff46ebfaaa371d18a2c3f16` |
| Firmware upload | Explicit COM26 upload, flash hash verified; boot idle |
| Modem | Responds to AT; firmware `UE8.2.1.0`, SVN19 |
| SIM | Ready after allowing initialization following the RF-off transition; no PIN attempts |
| RAT/APN | LTE-M, `iot.1nce.net`, no APN username/password |
| Identity/credentials | Newly provisioned1010 in the same household as hub16; independent bearer hash and Vault HMAC mapping enabled |
| TLS preparation | PC verified Supabase's live certificate chain to trusted GTS Root R4; modem accepted CA slot12 and CA+hostname-validating TLS profile2 |
| Software verification | Host tests cover credential gates, SIM readiness/PIN/timeout/cancellation, clock plausibility, profile cadence, NVS sequence reservation, packet HMAC and strict receipts |

TLS **configuration** is verified; a successful modem HTTPS handshake/cloud upload
is not implied by this row.

## Cellular tests

The first attempt reported registration denied and stopped before building or
uploading a packet. A subsequent test allowed the full 180-second registration
window, including transitions from denied back to searching. It still did not
register. The modem reported **RSRP -132 dBm** and **RSRQ -19.5 dB** at timeout and
returned to idle. Weak reception may contribute; it does not establish whether
the SIM subscription/roaming entitlement is correct.

The user was asked to confirm SIM activation/data availability in the 1NCE portal
and improve LTE antenna placement. Do not call this an end-to-end LTE success or
assume a modem/network rejection was a Supabase authentication failure.

## GNSS and cloud result

A separate GNSS-only attempt at15:29 UTC used the PC's current UTC to initialize
the receiver, with LTE off. It returned **NO FIX** after approximately86 seconds;
no synthetic coordinates, packet or upload was produced. Clear sky view/passive
GNSS antenna placement remains to be checked before repeating.

Supabase verification after the tests: device1010 has **zero observations**,
**zero positions** and `last_seen_at=null`. No actual Walter packet reached the
cloud, so hardware-packet comparison in the web workbench is still pending.

The final configured build was uploaded and hash-verified again, then inspected:
SIM ready, LTE-M selected, modem responsive and **idle with RF off**. COM26 was
released. Firmware usage: 64,172 bytes static RAM and412,061 bytes application
flash. The offline Walter suite and all15 web-console tests pass. No recurring
transmission session was left running.

## Firmware corrections from hardware testing

- Poll SIM readiness after CFUN changes, up to ten seconds. A PIN/PUK requirement
  stops immediately without attempting an unlock.
- Reject the observed default modem UTC in2070. The build timestamp only bounds
  plausible dates; it is never published as a substitute clock.
- Continue searching after a transient roaming rejection, while retaining a
  bounded registration window. Log actual state transitions and measured signal.
- Add credential-free inspection and fixed read-only raw diagnostics, available
  before the vendor driver starts. Raw diagnostics do not reset rejection history.
- Increase the USB transmit buffer and use DTR in the host terminal. Earlier
  captures dropped some completion/diagnostic text; missing text alone must not
  be treated as proof that the modem is still busy.
- Add explicit host UTC seeding and GNSS-only inspection so GNSS can be tested
  separately from network time acquisition. No synthetic coordinates are used.
- Log GNSS event status/satellites/accuracy/UTC for the next acquisition attempt.
  RAT changes now restart the modem and verify its refreshed RAT; the alternate
  NB-IoT path is still **not hardware verified**.

## Private local evidence

Factory backup, capture logs, the public CA chain record and credential bundle are
under ignored `.pio/walter-bench-20260828/`. Firmware credentials are in ignored
`collar/walter/include/walter_secrets.h`. Do not commit or publish these private
files or the configured firmware binary. The factory backup may also contain
private settings and must stay local.

No existing device credentials were rotated. No backend function/schema was
changed, and no PC/simulator telemetry was sent to make the hardware test appear
successful. Hardware reception, GNSS result and final cloud observation counts
must be checked before completing commissioning.
