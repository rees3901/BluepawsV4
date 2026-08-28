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

## Full-band LTE antenna, LTE-only retest, 19:00-19:04 UTC

The user fitted an antenna labelled698-960MHz and1710-2690MHz and explicitly
requested LTE only. Added an idle-only `lte` command: actual host UTC and device
credentials are required; it prepares SIM/APN/TLS, builds one signed no-fix report,
attempts registration/HTTPS once, then performs RF-off cleanup. It neither requests
GNSS nor calls the LoRa stub, even when a previous GNSS fix is cached. GNSS
configuration is now confined to the existing explicit GNSS/cycle paths. Bench
mode remains unchanged; this explicit diagnostic permits real LTE in bench mode.

- Walter host tests and all15 console tests passed. Build:64,188 bytes static RAM,
  415,205 bytes flash; uploaded only COM26 and verified flash hash.
- SIMREADY, PDP context/authentication, CA slot, TLS validation settings and HTTPS
  profile configuration all succeeded. This is not proof of SIM subscription
  activation, a working data session or an HTTPS handshake.
- LTE-M automatic registration in CFUN1 progressed0->2->3->2->0->2 and timed out
  after the180-second window. No home/roaming registered state was seen.
- CESQ measurements were unavailable, correctly rejected by the sentinel guard.
  No HTTP POST was attempted because registration failed. Supabase before/after:
  zero device1010 observations, zero positions and null last_seen_at.
- ESP packet1025:46 bytes, source1010/destination16, no valid coordinates,
  GNSS_VALID clear and ERROR_PRESENT set. HMAC verification and the live workbench
  parse-only API matched the exact bytes/hash; no simulator upload was substituted.
- RF-off cleanup succeeded. After an ESP-only reset, fixed read-only diagnostics
  confirmed CFUN0/CEREG0; CEER reported NO CAUSE RECEIVED for both EMM and ESM.
  SQNMONI could not supply a serving cell in RF-off mode (CME551). CPIN returned
  CME10 in this mode, not a contradiction of SIMREADY during RF-off initialization
  at CFUN4. Band profiles were readable; no band/RAT changes were made.
- Physical no-clock guard verified: `lte` refused to run after reset with UTC unset.
  Final status Normal/offline, busy=0/running=0, LoRa counters0; COM26 released.
  COM23/7/11 and PCB files untouched. GNSS was not exercised in this retest.

The new antenna did not establish LTE connectivity. Neither an antenna defect nor
an RF-front-end defect is proven. SIM activation/data allowance, LTE-M roaming
availability and RF installation/coverage remain open checks; keep PR142 draft.

Private evidence: `full-band-lte.log`, `full-band-lte-diagnostics.log`,
`full-band-lte-final-status.log`, `full-band-lte-packet.json`. Packet1025 SHA-256:
`2726f8b353538b4756b82a88b3829b2e0c1e7276594016db7527ed2189b97d1c`.

## Explicit authentication and alternate-RAT commissioning

The user confirmed the same 1NCE SIM worked in a Quectel modem immediately before
moving it to Walter. Treat that as established bench history, not an unresolved
SIM-activation question. Only COM26 was used; no GNSS, LoRa or cloud transmission
was requested during the following registration-only tests.

### Implemented and verified changes

- Host credential validation permits empty PAP username/password while retaining
  AT-command delimiter/control-character checks.
- The pinned WalterModem `setPDPAuthParams()` checks its cached NONE protocol
  before assigning the requested protocol and can return success without sending
  CGAUTH. The application now sends the validated command explicitly and propagates
  modem rejection. Earlier generic authentication-OK messages did not prove PAP.
- RAT changes now enter CFUN0 before SQNMODEACTIVE, reset the modem, refresh and
  verify the selected mode, then return to CFUN4 for SIM initialization.
- Fresh-boot `register ltem` / `register nbiot` commands own UART2, perform the
  vendor GPIO45 reset sequence, apply identical APN/authentication settings and
  allow five minutes of registration. They capture raw CEREG causes, CESQ and
  SQNMONI, plus CEER before RF-off cleanup. They restore the original RAT and
  verify CFUN0. No device clock, sequence number, TLS or HTTP is involved.
- An initial raw test timed out at ATE0 after ESP flashing. Vendor initialization
  recovered the modem and SIM; adding the same hardware reset pulse to the raw
  diagnostic resolved that startup problem. It was not a network rejection.

### Authentication result

The modem accepted `AT+CGDCONT=1,"IP","iot.1nce.net"`. On UE8.2.1.0, both empty-PAP
forms (`AT+CGAUTH=1,1,"",""` and `AT+CGAUTH=1,1`) failed with CME50. Consequently
the testbed retains authentication NONE and explicitly sends `AT+CGAUTH=1,0`,
which the modem accepted. No credentials were invented and no silent authentication
fallback was added. This is a measured firmware/modem compatibility constraint,
not evidence that 1NCE requires nonempty credentials.

### LTE-M result, 19:28-19:33 UTC

- SQNMODEACTIVE1 confirmed; active SQNCTM profile `standard`; SIM READY.
- APN and explicit CGAUTH accepted. Automatic network selection was used.
- CEREG included EMM cause15 (no suitable cells in tracking area), then cause19
  (ESM failure). A transient vendor state80 was also seen, but no stable
  registered-home/roaming state1/5 was reached in the five-minute window.
- Unlike earlier unknown measurements, SQNMONI repeatedly identified **3 UK,
  PLMN23420, band20, EARFCN6300**, with RSRP approximately -101 to -97.6dBm and
  RSRQ -15.2 to -13dB. This establishes reception of a cell, not a working bearer.
- Final live CEER: `lastEmmCause: ESM_FAILURE`,
  `lastEsmCause: OPERATOR_DETERMINED_BARRING`.
- Result NOT REGISTERED; no cloud transmission. Cleanup read back CFUN0 and
  left the original LTE-M RAT unchanged.

The explicit operator-barring result shifts the investigation toward network
authorization/roaming/device policy. It does not identify which policy caused
the refusal, and does not establish that all RF hardware is healthy. In particular,
a previously working SIM can still encounter a different RAT/roaming policy or
a device binding after being moved. Check 1NCE event logs and any existing IMEI
lock for the Walter attempt; do not change subscription/security settings blindly.

References: [Sequans AT manual](https://quickspot.io/docs/file/gm02s_at_commands.pdf),
[1NCE APN settings](https://help.1nce.com/dev-hub/docs/data-services-apn),
[ETSI TS24.301 ESM causes](https://www.etsi.org/deliver/etsi_TS/124300_124399/124301/12.06.00_60/ts_124301v120600p.pdf),
[1NCE IMEI lock and event logs](https://help.1nce.com/docs/1nce-portal/portal-sims-sms/).

### NB-IoT result, 19:34-19:39 UTC

- CFUN0 transition, SQNMODEACTIVE2 selection, reset and mode readback succeeded.
  The active profile remained `standard`; SIM READY and the same APN/CGAUTH
  settings were accepted.
- The five-minute window remained searching/unknown, with no registered state1/5.
  CESQ remained unknown255 and SQNMONI returned no usable cell (CME551/552).
- Final live CEER reported NO CAUSE RECEIVED for both EMM and ESM. This does not
  establish NB-IoT coverage or the reason no registration completed.
- Cleanup restored and read back SQNMODEACTIVE1 (LTE-M), then confirmed CFUN0.
  The worker returned idle and the serial capture closed COM26. No HTTP occurred.

Latest image:64,212 bytes static RAM and420,117 bytes flash; COM26 flash hash
verified. Host tests pass, including actual CGAUTH/RAT functions and observed
CEREG forms; all15 existing web-console tests pass. The raw UART registration
paths were exercised on the board, not exhaustively mocked in host tests.
No recurring RF test was left running. Other ports, PCB files, SIM-account settings
and backend state were not changed by these diagnostics. Keep PR142 draft;
neither stable registration nor cloud delivery has been established.

Private evidence under `.pio/walter-bench-20260828/`:
`pap-registration-ltem.log`, `pap-inspect-recovery.log`,
`pap-registration-ltem-reset.log`, `pap-registration-ltem-optional.log`,
`explicit-auth-registration-ltem.log`, `explicit-auth-registration-nbiot.log`,
`explicit-auth-final-status.log`.

## Outside-window GNSS and portal APN correction

### GNSS, 19:56-19:58 UTC

The user confirmed the module/antenna was now outside the window. Two GNSS-only
acquisitions were performed with LTE off and no upload:

- First: about69 seconds, status0,12 satellite entries, estimated confidence553.2m
  (rounded up to554m). Accepted by the existing1000m ceiling, but very coarse.
- Second, without a modem reset: about25 seconds, status0,6 satellite entries,
  estimated confidence68.7m (rounded up to69m). Accepted without relaxing checks.

These are receiver-estimated uncertainties, not independently measured positional
errors. Both events passed the existing time/coordinate/satellite/confidence
checks. No coordinates were fabricated or published. The host acquisition guard
was already180 seconds; the modem completed each single-fix request before it.
An additional acquisition, not a longer host timeout, supplied the improvement.

### Correct APN established from the user's portal

The user's portal screenshot specifies **sensor.net**, while this bench had used
the legacy **iot.1nce.net** value. The screenshot also shows global IMEI lock
disabled; it does not establish the individual SIM's lock setting. A user-supplied
1NCE INFO/location-update event at19:30:38.328Z correlated with the earlier Three
UK LTE-M test, but was not itself proof of a data session.

The earlier investigation used the wrong documentation variant for this account.
1NCE [Platform2 APN documentation](https://help.1nce.com/dev-hub/v200/docs/data-services-apn)
specifies sensor.net; the [migration guide](https://help.1nce.com/platform-migration/breaking-changes/)
retains legacy/custom APNs for existing SIMs. Always use the actual portal/SIM
configuration rather than assuming one provider-wide APN.

Changed only the local ignored Walter APN definition to sensor.net; bearer/HMAC,
TLS verification, IPv4 and explicit no-authentication settings were preserved.
Updated the example comment and commissioning guidance without committing secrets.
Build passed:64,212 bytes static RAM,420,101 bytes application flash. COM26 upload
hash verified. Walter host suite passed.

The subsequent LTE-M registration-only test reached CEREG5 (roaming), CGATT1,
and received a nonzero private IPv4 address. SQNMONI reported Vodafone UK/23415,
band20, RSRP-106.4dBm/RSRQ-19.8dB. CFUN0 cleanup succeeded. This proves registration
and packet attachment for this attempt, not HTTPS delivery. Because both physical
placement and APN changed since the earlier failure, it is not a controlled
single-variable RF/APN comparison; the APN mismatch itself is confirmed.

### Real HTTPS delivery, 20:00 UTC

Ran one explicit LTE-only upload after seeding current host UTC. No GNSS or LoRa
stub was requested. SIM/APN/CA/HTTPS configuration succeeded; registration reached
roaming. Signal estimate was RSRP-105dBm/RSRQ-17dB. TLS CA/hostname validation
remained enabled; no insecure retry or simulator/PC upload was used.

The modem returned HTTP201 with response_bytes=0. The current strict application
receipt check rejected the empty advertised response and reported DELIVERY
UNCONFIRMED; it did not automatically retry. The response-reading path remains
unverified and needs a separate fix/test, rather than treating201 alone as a
verified on-device receipt.

Independent Supabase reads established actual delivery: one device1010 observation,
sequence1281, path cellular_direct/link lte, matching the captured46 bytes exactly
in base64 and SHA-256:
`9927b93d0e1749de39baf4a1705097675de1621e25d07bc36fec4ad38de81210`.
last_seen_at advanced to2026-08-28T20:00:54.720276Z. Positions remained zero because
this explicit LTE diagnostic deliberately sent a no-fix packet; the earlier GNSS
fix was not included or claimed as a cloud position.

RF-off cleanup succeeded. Final status Normal/offline, busy=0/running=0,
LoRa count0; COM26 released. No other bench ports, PCB files, SIM-portal settings,
backend schema/functions or credentials were changed. PR142 remains draft for
receipt handling, combined GNSS-to-cloud testing and further accuracy validation.

Private logs: `outside-window-gnss.log`, `outside-window-gnss-repeat.log`,
`sensor-apn-registration-ltem.log`, `sensor-apn-lte-upload.log`,
`sensor-apn-cloud-comparison.json`, `sensor-apn-final-status.log`.

## Canonical credentials and complete GNSS-to-GUI path, 20:12 UTC

Merged the already-provisioned1010 bearer/HMAC entry into local `tools/devices.json`
after comparing both values with the flashed firmware. Preserved all five existing
collars and the gateway; kept a private backup. No key rotation or SQL provisioning
rerun. Both JSON/backup remain gitignored. Loaded the same bundle into the running
localhost8787 console only after verifying its existing entries matched the old
bundle; no unsaved credential changes or simulator reports were overwritten.

One online `send` cycle, with actual host UTC, acquired a fresh GNSS fix with
estimated confidence213.9m (packet214m),6 satellite entries. It built sequence1282,
simulated local LoRa completion/listening and sent the same46 bytes through LTE.
No LoRa transmission over air occurred. HTTP201 again advertised response length0;
the application retained delivery-unconfirmed status and did not automatically retry.

Supabase independently confirmed GNSS_VALID=true, sequence1282, cellular_direct/lte,
an observation, a position and a matching `device_latest_positions` entry in the
same Family as collar1001. Serial bytes and HMAC verification using the newly
merged canonical bundle matched both Supabase and the live standalone workbench.
SHA-256: `77b20bed3d6ecee883c0967ee56c3b9998dcc44cd440e6d1af787488ec78becf`.

The GUI uses `devices.display_name`, so device1010 appears as **Walter LTE Testbed**.
Code inspection also confirmed that the dashboard seeds its device list from
latest positions, not positionless presence alone. The user explicitly confirmed
the Walter card was visible. The agent's own available browser was signed out;
visual confirmation here is the user's, supplemented by database/code checks.

### User-requested approximately5m accuracy target

The214m transmission was already in flight when the user requested the tighter
target. No subsequent position uploads were made. Six GNSS-only settling checks
returned confidence205.2,202.3,198.1,101.3,101.0 and100.8 metres. The final three
included20-second gaps, without modem resets. **The5m target was not reached.**
No timeout/accuracy threshold was relaxed and no precision was fabricated. The
firmware still has its existing1000m validity ceiling; ordinary `send` is not a
5m-gated command. Further GNSS antenna/sky-view/assistance investigation is separate
from the now-demonstrated transport/display pipeline.

Final board status Normal, offline=1, busy=0/running=0, no scheduled transmissions;
COM26 released. GNSS-only diagnostics kept LTE off. No firmware change in this
follow-up; response-body handling remains open. No backend schema, account or
credential mutation, no changes to other physical bench ports or PCB files.

Private evidence: `gnss-lte-full-pipeline.log`, `gnss-lte-full-pipeline-comparison.json`,
`gnss-lte-workbench.json`, `precision-gnss-01.log`, `precision-gnss-02.log`,
`precision-gnss-03.log`, `precision-gnss-final-rounds.log`,
`full-pipeline-final-status.log`.
