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

## Follow-up: ESP offline LoRa transport bench

The user asked to continue ESP development independently of reception. This does
**not** establish an RF front-end fault: the command family is correct, but the
cause of failed registration remains open (including coverage/roaming/SIM state).

Added default-on offline bench mode. A current PC UTC seed permits real packet
construction/signing on the ESP without modem access. The LoRa stub returns a
successful local TX result, then the normal ten-second listen window runs. No hub
ACK is fabricated. LTE due events are logged and skipped while offline. Online
mode remains explicit (`bench off`), with real GNSS/LTE still uncommissioned.

Verified on COM26 with the final build:

- Reboot: idle, offline=1, UTC=0, counters zero; no automatic reporting.
- Sending without a clock: blocked without a packet/modem operation.
- With actual PC UTC: 46-byte signed packets, source1010, destination16.
- Captured sequences513/514/515: Normal INTERRUPT, Debug BOOT, Debug TELEMETRY.
  Each packet's hex/base64 matched; each HMAC verified with the private1010 key.
- Live workbench parse API on localhost8788 decoded the identical bytes and
  SHA-256 hashes. Only the parse endpoint was called; no credentials imported,
  simulator settings changed or telemetry uploaded to cloud.
- Debug repeat: approximately40 seconds between BOOT and next telemetry
  (ten-second listen plus thirty-second profile wait, plus diagnostic overhead).
- Stop during the third packet's listen window returned to idle in under3 seconds;
  three completed local TXs, two skipped LTE events. The cancelled third fallback
  did not run.
- Idle `bench off`/`bench on` switching verified without starting a cycle.
- ESP-only reboot returned to idle/offline/unset UTC. After reseeding the clock,
  the next sequence was769, skipping the unused NVS-reserved range after515.
- Final state: Normal profile, offline, idle; COM26 released. COM23/7/11 untouched.

Early captures exposed dropped characters in USB hardware CDC output. Merely
increasing the buffer or flushing each whole record was insufficient. A record
mutex plus paced32-byte writes produced complete captures in the tests above.
This is a bench logging workaround, not proof of unlimited USB reliability.

Final build:64,188 bytes static RAM;413,709 bytes application flash. Upload hash
verified. Expanded host tests execute the actual cycle/TX functions with fake
modem hooks, covering no-modem offline behavior, clock/stop/NVS gates, scheduled
fallback, and identical bytes passed to the online uploader after local TX.
All15 existing web-console tests pass. Physical LTE/GNSS was not retried here.

Private evidence remains under `.pio/walter-bench-20260828/`:
`offline-smoke.log`, `offline-packets.hex`, `offline-workbench-report.json`,
`offline-toggle.log`, `offline-reboot-status.log`, `offline-reboot-send.log`.

## Antenna-swap retest, 18:35-18:41 UTC

The user swapped the LTE/GNSS antennas and requested another real test. No
LoRa-over-air test was attempted. COM23/7/11 remained untouched.

- A GNSS-only run with fresh PC UTC and LTE off returned an event after about
  73 seconds: status0,12 satellite entries, estimated confidence20,000,000 metres.
  This is not a usable position and was rejected. The satellite entry count alone
  does not prove tracking quality or a navigation solution.
- One explicit online `send` repeated GNSS with the same unusable result. The
  ESP produced signed sequence770 with GNSS_VALID clear, ERROR_PRESENT set and
  no valid coordinates. Local workbench/HMAC verification passed; this is not
  evidence of LTE delivery.
- SIM, PDP/APN, GNSS configuration, trusted CA and HTTPS profile setup succeeded.
  LTE stayed in CFUN1, cycled through searching/registration denied, and did not
  reach registered-home or registered-roaming during the180-second window.
- At timeout, the pinned driver's CESQ conversion printed RSRP+115dBm and
  RSRQ+1080 tenths-dB. These are invalid measurements, consistent with its
  arithmetic conversion of raw255 (unknown). They cannot be compared with the
  earlier weak-signal measurement to claim the antennas improved/worsened RF.
- Supabase before and after: zero1010 observations, zero positions,
  last_seen_at=null. No HTTP upload succeeded or simulator substitute was sent.
- The cycle returned to idle after RF-off cleanup; `bench on` restored the safe
  offline mode. Further feature implementation remains pending real GNSS/LTE
  commissioning rather than assuming the antenna swap fixed it.

Fixed the confirmed CESQ diagnostic issue: values outside the pinned driver's
defined-code conversion ranges now display **Signal unavailable**. Host tests
cover the observed sentinel, mixed valid/unknown fields and both range boundaries.
No GNSS acceptance threshold was relaxed, and no modem/library firmware upgrade
was performed. The antenna fault cause remains unconfirmed; clear sky/passive
GNSS antenna and SIM/network registration checks are still needed.

Evidence: `antennas-swapped-gnss.log`, `antennas-swapped-send.log`,
`antennas-swapped-packet.json`, `antennas-swapped-final-status.log` under the same
ignored bench directory. Packet770 SHA-256:
`1df81838819492a7486270110980a523a6a0d4e9074c6b4935ff293d3052c1d6`.

Diagnostic-fix build and host tests passed; the COM26 upload hash was verified.
Final image uses64,188 bytes static RAM and413,813 bytes flash. Post-flash status
confirmed Normal profile, offline=1, busy=0/running=0, UTC unset and counters zero;
COM26 released. The new invalid-signal display is host-tested but was not used to
justify another RF retry. Post-flash log: `antennas-swapped-postflash-status.log`.
