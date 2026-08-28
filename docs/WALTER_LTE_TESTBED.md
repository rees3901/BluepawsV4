# Walter LTE/GNSS testbed

## Purpose and separation

Prepare an independent ESP32-S3 + Sequans GM02SP collar testbed while keeping the
working WisMesh nRF52840 + SX1262 LoRa testbed unchanged. The boards are not wired
together and must not share an identity or credentials.

| Bench device | Identity | Serial | Role |
| --- | --- | --- | --- |
| WisMesh board 1 | Existing collar 1001 | COM23, 115200 | Real LoRa; no GNSS/LTE hardware attached |
| LoRa home hub | Existing hub 16 / `0010` | COM7, 115200 | Receives LoRa and forwards to cloud |
| LoRa sniffer | Existing sniffer | COM11, 115200 | Passive RF observation |
| Walter | **1010**, separately provisioned | **COM26**, 115200 | Real GNSS and LTE; simulated LoRa TX |

The WisMesh build-time/spoof clock and missing-modem faults are expected on that
hardware. This work does not try to fix them. Device 1005 has already been used for
test data; confirm 1010 is still spare when provisioning.

The initial target was compile/offline tested before hardware arrived. Hardware
commissioning is now in progress on **COM26**, with a 1NCE SIM and separate device
1010 credentials. See [the dated bench record](WALTER_BENCH_2026_08_28.md) for results; do not infer LTE/GNSS
success merely from a successful firmware build. The existing TLV simulator and
workbench are unchanged.

## Build and architecture

From the canonical repository root:

```powershell
pio run -e walter
node tools/test_walter_testbed.mjs
```

The test requires Node 24 (native TypeScript stripping), a host C++17 compiler
(`CXX` override supported), and the ArduinoJson dependency installed by the build.
It compiles the actual policy/packet/HTTP helpers and passes a generated packet
through both the web workbench decoder and Supabase's ingestion parser. It sends
no network requests and opens no serial ports. Fixtures go under ignored
`.pio/walter-tests/` and contain **synthetic** keys/data only.

Files are isolated under `collar/walter/`, with a dedicated board definition at
`boards/dptechnics_walter.json`. Existing default environments are unchanged.
The target uses PlatformIO `espressif32@6.12.0` / Arduino ESP32 2.0.17, C++17,
16 MB QIO flash, 2 MB QSPI PSRAM and hardware USB CDC. It uses FreeRTOS, but the
Nordic BSP, filesystem, serial/modem adapter and peripheral drivers are not binary
or source interchangeable with ESP32. Only the shared wire format and profile
configuration are reused directly.

One FreeRTOS worker owns all modem operations. USB serial stays responsive in the
Arduino task; vendor event callbacks only copy results into queues. No Wi-Fi
connection or SX1262 driver is started. UART2 uses WalterModem's board defaults:
RX14, TX48, RTS21, CTS47, modem reset45, 115200 baud. These are internal modem pins,
not a reason to wire the two testbeds together.

### Cycle

1. Boot idle in **offline bench mode**. An explicit `start` or `send` is required
   after every ESP reboot. Offline cycles use an explicitly seeded current host
   UTC, require the packet HMAC key, and never initialize/query the modem. Missing
   UTC blocks the packet instead of inventing a clock.
2. For real RF commissioning, explicitly select `bench off` while idle. Online
   mode requires separate APN, bearer, HMAC and CA credentials. Modem/APN/TLS
   setup is deferred until network time or an LTE upload is needed; no SIM PIN
   is guessed.
3. Online only: if necessary, register once to acquire network UTC. A plausibility guard rejects
   the observed default modem date in 2070. Build time only bounds accepted dates
   (one day before compilation through five years afterward); rebuild an older
   test image before using it beyond that window. No compile timestamp or
   invented location is substituted. Without usable UTC, packets are blocked.
4. In online mode, deregister/put LTE in minimum mode, then acquire GNSS.
   The modem cannot operate
   GNSS and LTE concurrently. A failed GNSS cancellation stops the session before
   starting LTE. Fix acceptance requires valid UTC, finite/in-range coordinates,
   at least four reported satellites and estimated accuracy in `(0, 1000]` metres.
   Offline mode skips GNSS and supplies no fix: no coordinates or GNSS-valid flag
   are invented. GPS-bearing reports therefore retain the normal no-fix error.
5. Build and sign **one immutable TLV v1.2 packet**. The LoRa transport stub
   returns success to the ESP cycle and logs `TX_COMPLETE result=OK`, hex and
   base64, then waits the shared 10-second
   command-listen window. No real radio reception, hub acknowledgement or cloud
   LoRa observation is fabricated.
6. When the selected policy calls for LTE, offline mode logs **fallback due / skipped**
   and advances the simulated LTE schedule. It does not set a cellular failure
   merely because the test deliberately skipped RF, nor does it claim acceptance.
   In online mode, configure/reactivate LTE and POST the **same
   packet bytes** to the project's `ingest-position` function, wrapped as
   `ingest_path=cellular_direct`, `link_type=lte`, `payload_b64=...`.
7. Confirm a successful HTTP status and a JSON receipt with `accepted=true`,
   matching device ID, sequence, SHA-256 payload hash and cellular transport.
   A modem `OK`, HTTP 200 alone, or a receipt for another packet is not acceptance.
   Duplicate acceptance is valid. There is no automatic retry in this first target;
   a timeout means delivery is **unconfirmed**, not proof the server rejected it.
8. Online: close HTTP after an upload attempt, put the modem in minimum mode and wait
   until the next cycle. Stop the session if radio-off cannot be confirmed.

### Shared profile policy

| Profile | Wait after cycle | Away LTE attempt | Home LTE heartbeat | Home GNSS refresh |
| --- | --- | --- | --- | --- |
| Normal | 600 s | Every 10 cycles | 3600 s | Every 10 home cycles |
| PowerSave | 1800 s | Every 30 cycles | 10800 s | Every 10 home cycles |
| Active | 60 s | Every 5 cycles | 600 s | Every 10 home cycles |
| Debug | 30 s | Every cycle | 30 s | Every home cycle |
| Lost | 30 s | Every 3 cycles | Home gating bypassed | Every cycle |

The first `start` cycle forces a BOOT LTE report; `send` forces one INTERRUPT
report. Both count as a cycle. PowerSave home check-ins occur every second home
cycle; other profiles check in every home cycle. Home heartbeats are timed from
the last LTE **attempt**, regardless of acceptance. These values come from
`bp_config.h`, not a second copied policy table. Acquisition/registration time is
additional to the wait, so this is not an exact wall-clock transmission schedule.
Lost falls back to Active after two hours, checked between cycles.

### Truthful packet fields

- Fresh GNSS sets `GNSS_VALID`, but does **not** invent `FIX_3D`; the vendor event
  does not provide a confirmed fix-dimension field. A fix aged 60 seconds or more
  is stale. Invalid/stale fixes do not publish coordinates as valid.
- Battery `0` means unmeasured on this target. No battery ADC, RF metrics, reset
  reason or boot-loop count is fabricated.
- GNSS acquisition failure and the previous unconfirmed cellular attempt set the
  generic error flag. The existing shared TLV contains no specific cellular-error
  code, so the detailed modem failure remains in serial diagnostics. Successful
  cellular acceptance clears that latch for subsequent packets.
- Home is an explicit serial-controlled simulation, not BLE detection. Home
  wake-checkins intentionally omit a fix, without declaring that omission a GPS
  acquisition failure. The destination remains affiliated hub16 in the signed
  packet; the HTTPS wrapper identifies the actual cellular route.
- NVS reserves sequence numbers in blocks of 256 before use. Reboots skip unused
  numbers instead of reusing the current block. The wire sequence is still 16-bit
  and eventually wraps; it is not a globally unique receipt identifier by itself.

## Provisioning and repeat setup

Device1010 is now provisioned for the COM26 board. Reuse its private local
credentials for routine rebuilds; do not generate replacement keys or rerun its
provisioning SQL. The following checklist also describes setup for another board,
which must use a different spare identity.

1. Confirm the selected spare device ID and its owner/animal/hub association.
   Follow the existing [TLV ingestion runbook](TLV_INGESTION_RUNBOOK.md) to provision
   a **new per-device bearer token and independent 32-byte HMAC key**. Do not reuse
   collar1001/simulator credentials. Do not put a Supabase service-role/secret key
   in firmware. The existing endpoint validates its own device bearer and HMAC;
   no endpoint auth changes are required by this target.
2. Copy `collar/walter/include/walter_secrets.example.h` to `walter_secrets.h` in the
   same directory. The destination is gitignored. Fill all 32 HMAC bytes, bearer,
   selected ID, affiliated hub ID, SIM APN, APN authentication and RAT. The sample
   intentionally cannot transmit. This firmware does not automatically unlock
   PIN-protected SIMs.
3. Verify the current TLS certificate chain for
   `ykcdaonkvwemedotdpdr.supabase.co` and supply its trusted public root CA PEM.
   Do not assume an example certificate is correct or disable validation to get
   a connection working. TLS1.2 validates **both CA and hostname**. First explicit
   modem setup after each ESP boot writes the configured public CA to modem slot12
   and configures TLS profile2. These persistent modem settings overwrite those
   selected slots; reserve them for this testbed. Slots0–10 and BlueCherry profile1
   are not used. Credentials remain embedded in firmware: no production flash
   encryption/secure provisioning claim is made.
4. With power disconnected, install a compatible LTE-M/NB-IoT SIM and the proper
   LTE antenna; attach a **passive** GNSS antenna with clear sky view. Check the
   SIM's carrier coverage, RAT and APN; a generic phone/data SIM is not automatically
   usable for these IoT RATs. Follow the board vendor's power and antenna guidance.
5. Connect Walter and positively identify its **new USB COM port**. Do not assume
   COM23/7/11: those belong to the working bench. Rebuild with the local secrets,
   inspect the target/port, then upload only when ready:

   ```powershell
   pio run -e walter -t upload --upload-port COM_NEW
   pio device monitor --port COM_NEW --baud 115200
   ```

   Replace `COM_NEW` with the identified Walter port. Do not run those placeholders
   literally, run an unqualified upload, or erase the existing boards. This target
   uses the standard 16 MB Arduino partition table, not the vendor's MOTA layout;
   modem OTA/BlueCherry/CoAP/MQTT/socket features are disabled. Modem firmware update
   or full-flash erase is a separate, explicitly planned operation.
6. For online commissioning, start with `status`, `bench off`, `profile debug`,
   then `send`. Capture the serial hex and
   acceptance hash. Paste **the hex portion after `hex=`** into the existing web
   console's Packet Workbench; select the new device's key explicitly (or parse
   without HMAC validation). No need to send a simulator report.
7. Compare device1010's `cellular_direct` observation in Supabase against raw bytes,
   sequence, UTC, payload hash, GNSS flags/coordinates and signature. Confirm the
   sniffer/hub saw **no Walter LoRa packet**. A real valid fix should update its own
   position; a no-fix report may be accepted without a position update.
8. Test `start`/`stop`, unplug/reboot idle behavior, independent credentials, home
   simulation and each profile cadence. Record modem firmware, SIM/RAT/APN (without
   secrets), port, signal conditions and outcomes before calling this commissioned.

## Serial commands and limitations

| Command | Effect |
| --- | --- |
| `help` | List commands while idle (also the fallback for an unrecognised line) |
| `status` | Identity, profile, busy/running, simulated home and configuration presence; no secrets |
| `bench on` / `bench off` | Default on after reboot: offline cycles skip all modem access; off explicitly enables real GNSS/LTE in the next cycle |
| `inspect` | Initialize modem, select RF-off mode and inspect SIM readiness, modem SVN, RAT and raw clock; no telemetry |
| `diagnose` | Before any modem initialization after ESP boot: fixed read-only AT queries for version, operational state, registration, rejection history and bands |
| `clock <UTC epoch>` | Explicitly seed actual host UTC while idle; no invented GNSS fix |
| `gnss` | With a valid UTC seed, test real GNSS with LTE off; no packet or upload |
| `assist` | With valid UTC and configured modem credentials, inspect GNSS assistance and download missing/due almanac or realtime ephemeris over LTE; verify expiry and return RF-off/idle; no telemetry |
| `lte` | With actual host UTC and credentials, register on LTE and attempt one signed no-fix upload; no GNSS or LoRa stub; returns to RF-off/idle |
| `register ltem` / `register nbiot` | Before library initialization: reset modem, apply APN/authentication, compare registration for up to five minutes, capture raw diagnostics, restore original RAT and RF off; no GNSS/LoRa/HTTP |
| `send` | One forced-report cycle, then idle; LTE is skipped in offline mode |
| `start` | BOOT cycle, then recurring profile policy |
| `stop` | Cancel pending work/interrupt waits and clean up modem state |
| `profile normal`, `powersave`, `active`, `lost`, `debug` | Select profile while idle; include `profile ` before the name |
| `home on` / `home off` | Set simulated home condition while idle |

Wait for `Idle` before restarting or changing settings. Stop cannot retract an
already transmitted request or immediately interrupt a blocking vendor AT call.
Network and GNSS waits each allow 180 seconds, HTTP event wait 130 seconds, plus
vendor command timeouts. A cleanup failure is printed, stops further scheduling,
and requires checking the board; do not assume RF is off after such a failure.
An ESP-only reset does not guarantee that an independently powered modem reset:
boot-idle means no application-initiated traffic, not proof of modem RF state.

### Work on the ESP without LTE/GNSS reception

On COM26 at 115200, send `clock <current UTC epoch>` (from
`[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()` on the PC), then `send`. The default
`bench on` mode produces a real signed packet on the ESP without any modem calls.
`start` exercises normal profile scheduling; `stop` cancels the listen/sleep wait.
`status` shows `offline`, current UTC, completed stub TX count and skipped LTE count.
No changes to the simulator's reports or sequence settings are required.

The simulated transmit function returns success just as a completed local radio
send would, but cannot prove reception. The ten-second listen window still runs;
there is no fabricated hub ACK, downlink command or cloud receipt. Normal profile
LTE ratios/heartbeats are preserved, using a simulated schedule when offline.
An online cycle with host UTC can also build its LoRa packet after GNSS failure;
APN/TLS preparation is deferred until LTE is actually due.

For LTE antenna testing alone, seed `clock <current UTC epoch>` then use `lte`.
This deliberately reports no valid coordinates, even if an earlier GNSS fix is
cached. It leaves bench mode and LoRa counters unchanged. The normal `send` command
still follows the complete telemetry cycle and can request GNSS when online.

Explicit diagnostic commands (`inspect`, `diagnose`, `gnss`, `assist`, `lte`, `register`) remain real modem
operations even in bench mode. Offline only suppresses modem operations in the
telemetry cycle. It does not forcibly switch off an independently running modem;
check any prior RF-off failure before treating the physical board as RF-inactive.
Online mode, clock and running state are not persisted across an ESP reboot.

SIM initialization can lag a transition to RF-off mode. The firmware uses a
ten-second polling window (plus any in-flight vendor AT timeout), stopping
immediately if a PIN/PUK is requested; it never attempts
to unlock the SIM. Setup diagnostics identify the failing stage without printing
credentials. Copy the APN from the actual SIM's portal, not a generic provider
default. This bench's portal specifies `sensor.net` (1NCE Platform2); legacy SIMs
may retain `iot.1nce.net`. LTE-M is selected for this bench.
[Platform2 APN guidance](https://help.1nce.com/dev-hub/v200/docs/data-services-apn)
and [migration compatibility](https://help.1nce.com/platform-migration/breaking-changes/).

Empty PAP credentials are accepted by host validation, but this board's UE8.2.1.0
firmware rejects both omitted and quoted-empty credential forms with CME50.
The bench therefore retains explicit no-authentication mode; no invented
username/password or silent authentication fallback. The application sends
CGAUTH explicitly because the pinned driver's authentication setter returns early
when its cached protocol is NONE, without applying the requested setting. User and
password validation still rejects command delimiters/control characters. RAT
changes use CFUN0, then restart and verify the modem's mode before SIM setup.

The registration-only commands own UART2 without WalterModem tasks; they refuse
to run after `inspect`, `gnss`, `lte` or an online cycle initialized the library.
Reboot the ESP first in that case. Each registration test resets the modem and
captures only allowlisted diagnostic responses, never authentication echoes.
Read-only `diagnose` still preserves rejection history by not resetting the modem.
Registration tests report raw CESQ codes (255 means unknown), active operator
profile, CEREG details, CEER and serving-cell readings while RF is active. They do
not require a host clock, consume packet sequences or contact Supabase. Both RATs
use the same APN/authentication settings and five-minute search window. Stop cancels waits;
cleanup attempts CFUN0 and restores/verifies the original RAT even after stop.
Any unconfirmed cleanup requires checking the board. A passed registration test
does not prove HTTPS or delivery.

The registration loop logs state changes and allows the full timeout even when an
individual roaming network rejects registration. It only considers home/roaming
registered states successful. A denied state followed by searching is not itself
a completed connection. On timeout, measured RSRP and RSRQ are printed when the
modem supplies them; they are not fabricated into the TLV.

For independent GNSS testing, use the host's **current** UTC rather than copying an
old timestamp. This is an explicit bench clock source, not a GNSS fix or a test of
network time acquisition. Serial diagnostics use a 4 KB USB transmit buffer;
assert DTR in the host terminal for reliable logging. No unrestricted AT console
or credential-reading command is exposed.

COM26 testing also exposed dropped characters in rapid HWCDC writes. Application
diagnostics now format whole records under a mutex and pace 32-byte chunks by
5 ms, without concurrent `flush()` calls. This is a low-volume bench workaround,
not a throughput guarantee; validate hex/base64 and HMAC before using a capture.
Related upstream reports: [ESP32 HWCDC missing data](https://github.com/espressif/arduino-esp32/issues/9378).

This is a transport/GNSS bench, **not production power validation**: the ESP uses
interruptible FreeRTOS waits rather than deep sleep, and PSM/eDRX are not enabled.
There is no BLE scanning, buzzer, geofence engine or battery sampling. The LTE
command window supports profile changes and Lost Alert entry/exit, with signed
TLV ACKs; other command types are deliberately not executed or ACKed.
Home status, LoRa delivery and battery life therefore cannot be validated with
Walter alone. End-to-end RF, TLS, modem firmware compatibility, antenna performance
and actual cloud acceptance remain hardware commissioning checks.

### LTE command window

After a matching upload receipt, Walter keeps LTE available for at least
`CMD_LISTEN_WINDOW_MS` (10 seconds). It handles a command in that receipt and
polls the same HTTPS endpoint with `format=device_commands`,
`ingest_path=cellular_direct`, and its numeric `device_id`. The existing device
bearer authenticates each poll; no Supabase user/service key is put on the board.
Polling does not invent observations, refresh last-seen, or ACK a command.

There is a final poll at the deadline. HTTP requests and signed ACK uploads can
extend the window beyond ten seconds, bounded by existing modem/HTTP timeouts.
`stop` cancels waits; a blocking vendor AT call may finish before cancellation.
GNSS stays off throughout. The caller closes HTTP and turns RF off afterward.

Only `set_profile`, `enter_lost_alert`, and `exit_lost_alert` are implemented.
The target device, command UUID, nonzero 16-bit sequence, type/profile and UTC
expiry are validated before applying a command. ACKs use a newly reserved
packet sequence, `TX_ACK`, and `TLV_ACKED_MSG_SEQ_ID`, signed with the existing
collar HMAC key. A confirmed packet receipt and confirmed command ACK are logged
separately. Unsupported/expired commands are never falsely acknowledged.
Duplicate delivery during a boot is re-ACKed without restarting Lost Alert.
Profiles and duplicate-command memory remain volatile, as with the existing
serial profile setting; reboot returns to Normal/offline bench mode. This is
not durable production command execution across power loss.

The pinned modem library caps HTTP reads at 1500 bytes. A zero-length HTTP RING
event now triggers a bounded body read, because an absent content length is not
proof of an empty response. Empty, truncated, invalid JSON or mismatched receipts
still fail. The August 28 live run confirmed matching receipts despite the modem
advertising zero response bytes, including signed command ACKs. See the bench record.

Deploy the updated `ingest-position` function before flashing this firmware.
Older endpoints reject the new poll format; telemetry acceptance remains valid,
but a rejected poll is reported as unavailable command delivery.

For a manual test on COM26 at 115200 (DTR enabled), send `status`, seed actual UTC
after a reset using `clock <epoch>`, then use `gnss` for position only. PowerShell
`[DateTimeOffset]::UtcNow.ToUnixTimeSeconds()` supplies the current epoch.
`bench off` then `send` runs GNSS, simulated LoRa and real LTE; **it still accepts
estimated accuracy up to 1000 m and does not enforce a 5 m upload threshold**.
`lte` tests the command window without acquiring/uploading coordinates. Queue a
profile for device 1010 in the web UI and verify `APPLIED`, a signed ACK upload,
`CLOUD ACK CONFIRMED`, and the matching database command changing to `acked`.
Commands queued after the window remain pending until the next LTE check-in.

### GNSS assistance diagnostic

If GNSS snapshots remain coarse, seed current host UTC, run `assist`, and wait for
`Idle` before `gnss` or `bench off` / `send`. This explicit command checks almanac
and realtime ephemeris availability/update times, registers on LTE only if an
update is due, waits up to 60 seconds for each download event, then verifies both
items are available and unexpired. Registration and vendor AT timeouts are
additional. `stop` cancels the event wait. Cleanup attempts RF-off even on failure;
check any cleanup warning before continuing. No packet sequence, telemetry,
profile command or simulated LoRa transmission is generated by `assist`.

This follows the pinned vendor positioning example's assistance APIs. It does
not automatically refresh on every cycle or guarantee a particular accuracy.
GNSS and LTE still run separately. The existing 180-second acquisition guard
and 1000 m validity ceiling are unchanged; there is no 5 m requirement.
On August 28, an expired almanac was refreshed, followed by 107.3 m and 77.4 m
receiver uncertainty estimates. This sequence is evidence of recovery, not a
controlled test proving assistance alone caused the improvement.

## Home-hub distance display

The dashboard and shared search map no longer measure from a hardcoded London
coordinate. The `device_latest_positions_with_home` invoker view joins a report's
logical destination to the same Family's `hub_presence`. This works regardless
of whether the immutable packet arrives over LoRa or direct LTE. A legacy,
cloud-addressed or broadcast packet uses a Family home hub only if there is
exactly one; an explicit unknown/foreign destination never falls back to another
hub. No hub/fix or an ambiguous home means `Unknown`, not zero or London.

Distance is calculated from the latest stored coordinates, not inferred from
the HOME beacon flag. Hub movement updates the dashboard via existing presence
refreshes and the shared map via its next snapshot. Coordinates and distances
remain estimates: a nearby physical board with a stale/simulated GNSS coordinate
can still show a nonzero distance. Apply migration
`20260828203228_home_hub_distance.sql` before deploying the web application.

## Official documentation and pinned dependencies

- [Walter datasheet](https://quickspot.io/datasheet/walter_datasheet.pdf) and
  [documentation index](https://www.quickspot.io/documentation.html): board hardware.
- [Arduino setup](https://github.com/QuickSpot/walter-documentation/blob/main/developer-toolchains/arduino.md):
  ESP32-S3 USB/flash/PSRAM settings.
- [WalterModem source](https://github.com/QuickSpot/walter-arduino/tree/8debb2155ad4e1bfacd2063bbc00dd2256beedb6):
  pinned to `8debb2155ad4e1bfacd2063bbc00dd2256beedb6`, not floating main.
  Version1.5.0 lacks the needed POST extra-header parameter; the pinned revision
  includes the later Authorization-header support and its fixes.
- [TLS/certificate reference](https://github.com/QuickSpot/walter-documentation/blob/main/walter-modem/arduino_esp-idf/reference/tls_and_certificates.md)
  and [GNSS FAQ](https://github.com/QuickSpot/walter-documentation/blob/main/faq/gnss-gps.md):
  CA slots, validation, antennas and GNSS/LTE exclusion.
- [Vendor license](https://github.com/QuickSpot/walter-arduino/blob/8debb2155ad4e1bfacd2063bbc00dd2256beedb6/LICENSE):
  the DPTechnics library is licensed for Walter boards. Do not transplant this
  dependency into the final custom Nordic collar; keep the adapter isolated.

Reviewed 2026-08-28. ArduinoJson is pinned to 7.4.3. Revisit the driver pin when a
tagged release includes these HTTP changes; rebuild and repeat bench verification
before upgrading.

## Registration investigation: documentation review, 2026-08-28

Useful primary references:

- [Walter getting started](https://github.com/QuickSpot/walter-documentation/blob/main/guides/getting_started.md)
- [Walter cellular connectivity guide](https://github.com/QuickSpot/walter-documentation/blob/main/guides/cellular_connectivity.md)
- [Sequans LR8.2 AT Commands Reference, Rev.3](https://quickspot.io/docs/file/gm02s_at_commands.pdf)
- [1NCE APN settings](https://help.1nce.com/dev-hub/docs/data-services-apn)
- [1NCE SIM status/session/IMEI-lock reference](https://help.1nce.com/docs/1nce-portal/portal-sims-sms/)

Sequans' separate EVK material is distributed through its documentation portal;
[Sequans support reports migration to SharePoint, Mass Market folder](https://forum.sequans.com/t/upgrade-path-for-gm02sp-ue8-2-0-3/433/6).
No separate publicly accessible GM02SP EVK getting-started manual was verified.
The full512-page AT manual above was downloaded and the relevant pages rendered
locally because its embedded text extraction is unreliable. Private working copy:
`.pio/walter-bench-20260828/gm02s_at_commands.pdf` (not vendored into Git).

Findings against firmware at review time (subsequent changes are noted below):

1. Walter's cellular guide records poor UK LTE-M coverage reported with Soracom
   SIMs and suggests NB-IoT. This is a reason for a controlled alternate-RAT test,
   not proof about this1NCE SIM. 1NCE advertises both technologies in the UK;
   coverage/roaming for a particular network and location remains unverified.
2. Sequans printed page90 requires CFUN0 before changing SQNMODEACTIVE. Our
   `prepareModem()` conditional RAT-change path currently starts from CFUN4 and
   needs an explicit CFUN0 transition. This path did not run in the last LTE-M
   attempt, so the discrepancy does not explain that failure by itself.
3. 1NCE specifies IPv4 and iot.1nce.net, which match our configuration. It recommends
   PAP if authentication must be selected, with empty username/password. Current
   firmware selects no authentication, and `credentialsReady()` incorrectly rejects
   empty PAP credentials. Allow that provider-supported combination before testing
   it; do not claim the difference caused the registration failure without evidence.
4. Printed pages315-316 describe optional CEREG rejection fields; our driver enables
   mode5 but does not retain those causes. Capture an allowlisted raw registration
   response and CEER/SQNMONI while the failure is live, before CFUN0 cleanup.
   Printed page40 identifies SQNCTM? as the active operator-profile query; the
   existing SQNBANDSEL listing alone does not establish which profile is active.

Checks proposed at review time: read active modem/profile settings; verify the SIM's
Activated state, allowance and IMEI lock in1NCE; align the APN authentication
configuration, then compare registration-only LTE-M/NB-IoT attempts one variable
at a time. Keep GNSS, LoRa and cloud uploads out of this diagnostic comparison.
Do not repeatedly reset/reconnect: [Walter's communication FAQ](https://github.com/QuickSpot/walter-documentation/blob/main/faq/communication.md)
warns against more than3-6 reconnections/hour. A three-minute timeout is an
application limit, not a hardware-failure verdict; the getting-started guide's
ten-minute demo allowance includes GNSS and must not be presented as an LTE-only
registration timeout.

This review made no firmware, modem configuration, SIM account or hardware changes.

Subsequent implementation: PAP empty-credential validation, explicit CGAUTH,
CFUN0 RAT switching and registration-only raw diagnostics are now implemented.
See the dated bench record for actual tests; these changes alone do not establish
that the original registration problem is fixed. The user confirms the same SIM
worked in a Quectel modem before moving it to Walter; no SIM-account changes are
needed merely to repeat that check.

The subsequent five-minute LTE-M test identified Three UK/23420 on band20
(RSRP about -101 to -97.6dBm), but ended with CEER ESM_FAILURE /
OPERATOR_DETERMINED_BARRING. NB-IoT also timed out, with no usable cell measurement
or rejection cause. Both used explicit accepted IPv4 APN/no-authentication
configuration. Original LTE-M selection and CFUN0 were restored and verified.
Provider event logs and any existing Quectel IMEI binding are the next targeted
checks; an RF-front-end defect or particular account restriction is not proven.
No cloud upload was attempted in this comparison.

Later portal evidence established that this account uses **sensor.net**, not the
legacy APN used in those failed attempts. After correcting the local APN and
moving the antenna outside the window, LTE-M registered and a real HTTPS packet
was independently matched in Supabase (device1010, sequence1281). The modem
reported HTTP201/response length0, so the firmware's strict receipt check still
reported unconfirmed at that point; the later receipt-body fix and full LTE
command round trip are now hardware verified in the dated bench record. Separate
GNSS acquisitions improved from553.2m to68.7m estimated confidence without changing
the180-second timeout or validity thresholds. See the dated bench record; the
successful LTE-only packet intentionally contained no position.
