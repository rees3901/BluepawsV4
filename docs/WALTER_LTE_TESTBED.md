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
| `status` | Identity, profile, busy/running, simulated home and configuration presence; no secrets |
| `bench on` / `bench off` | Default on after reboot: offline cycles skip all modem access; off explicitly enables real GNSS/LTE in the next cycle |
| `inspect` | Initialize modem, select RF-off mode and inspect SIM readiness, modem SVN, RAT and raw clock; no telemetry |
| `diagnose` | Before any modem initialization after ESP boot: fixed read-only AT queries for version, operational state, registration, rejection history and bands |
| `clock <UTC epoch>` | Explicitly seed actual host UTC while idle; no invented GNSS fix |
| `gnss` | With a valid UTC seed, test real GNSS with LTE off; no packet or upload |
| `lte` | With actual host UTC and credentials, register on LTE and attempt one signed no-fix upload; no GNSS or LoRa stub; returns to RF-off/idle |
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

Explicit diagnostic commands (`inspect`, `diagnose`, `gnss`, `lte`) remain real modem
operations even in bench mode. Offline only suppresses modem operations in the
telemetry cycle. It does not forcibly switch off an independently running modem;
check any prior RF-off failure before treating the physical board as RF-inactive.
Online mode, clock and running state are not persisted across an ESP reboot.

SIM initialization can lag a transition to RF-off mode. The firmware uses a
ten-second polling window (plus any in-flight vendor AT timeout), stopping
immediately if a PIN/PUK is requested; it never attempts
to unlock the SIM. Setup diagnostics identify the failing stage without printing
credentials. 1NCE's published APN is `iot.1nce.net`; LTE-M is selected for this
bench. [1NCE APN guidance](https://help.1nce.com/dev-hub/docs/data-services-apn).

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
There is no BLE scanning, buzzer, geofence engine, battery sampling or command
execution/ACK path. Cloud pending commands are reported but never falsely ACKed.
Home status, LoRa delivery and battery life therefore cannot be validated with
Walter alone. End-to-end RF, TLS, modem firmware compatibility, antenna performance
and actual cloud acceptance remain hardware commissioning checks.

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
