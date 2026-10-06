# Personal XIAO ESP32-S3 collars

Isolated personal firmware compatible with the existing BluePaws V4 platform.
This branch is not for merging into `main`. Canonical firmware, protocol headers,
web/backend sources, root build configuration and database schema stay unchanged.
The one approved configuration exception is a rule in `web/vercel.json` disabling
deployments for this personal branch only. Never merge this branch into `main`.

## Hardware and references

- XIAO **ESP32-S3**, Wio SX1262 **B2B** edition, L76K GNSS, no cellular modem.
- Radio GPIO: NSS 41, SCK 7, MOSI 9, MISO 8, reset 42, busy 40, DIO1 39.
- L76K: RX GPIO44 (D7), TX GPIO43 (D6), WAKE_UP GPIO1 (D0), UART 9600.
- User button GPIO21; legacy assembly indicator GPIO48. Do not replace this with
  `LED_BUILTIN`: the installed XIAO board package assigns that to GPIO21.
- No verified battery divider, fuel gauge or buzzer is fitted in this profile.
- WAKE_UP requests standby; it does not disconnect the GNSS supply.

Hardware reference: [legacy transmitter at 1b2ef636](https://github.com/rees3901/BluePawzTransmitter/tree/1b2ef636452352916b01f335ac2f09f02006e1c1).
Vendor references: [ESP32-S3 B2B kit](https://wiki.seeedstudio.com/wio_sx1262_with_xiao_esp32s3_kit/),
[L76K module and protocol documents](https://wiki.seeedstudio.com/get_start_l76k_gnss/).
Radio parameters and runtime profiles come from the unchanged V4 `bp_config.h`,
not the legacy 868 MHz JSON protocol. This image cannot communicate with V3 hubs.

## Behaviour and compatibility limits

- HMAC-SHA256 authenticated V4 TLV v1.2 uplinks, addressed to one provisioned hub.
  Neither V3 JSON nor a device MAC-derived identity is transmitted.
- Real GNSS only. Every acquisition allows at most 60 seconds total, including
  receiver wake and 10 seconds of continuous stabilization. A fresh location,
  at least four satellites and HDOP <= 5 are required; quality loss restarts
  stabilization without extending the deadline. Valid NMEA UTC sets
  the RTC; there is no fabricated build-time clock. No uplink is sent until UTC
  is available. Deep sleep retains the last fix and clock. Cold power loss requires
  time acquisition again. Until a first position exists, every wake attempts GNSS
  even when the Home beacon is present. Invalid/stale coordinates never carry GNSS_VALID.
- Serial NMEA diagnostics summarize checksum-valid GGA quality, GSA fix type,
  RMC validity, GSV visible satellites/maximum SNR and receiver TXT messages.
  Coordinates are not dumped. GSV visibility is the latest message, not a total
  across constellations. First-NMEA and first-good-fix timing are measured each
  attempt. An old retained collar fix does not establish receiver hot/warm start.
  NMEA does not establish backup-cell voltage or failure; check V_BCKP electrically
  with main power removed if repeated slow starts persist. No cold-reset command
  is sent. A timeout reports the true failure and retains the previous position.
- BLE scans for the existing `BLUEPAWS_HOME` advertisement, with a -90 dBm gate.
  Two missed scans clear Home. The first miss still counts as a scheduled Home
  wake and sends any due check-in, with the fresh-beacon-seen flag cleared.
  Optional `PERSONAL_HOME_BLE_ADDRESS` restricts it
  to the personal hub's observed BLE address; the canonical beacon has no hub ID.
- Every hardware wake sends a signed `WAKE_CHECKIN` presence packet before BLE
  scanning or GNSS acquisition, including single-button and LED-only timer wakes.
  It omits GPS, stale-fix and GPS-error flags and uses the previously retained
  home state; HOME_BEACON_SEEN stays clear until a new scan actually hears it.
  Zero packet time means UTC unavailable, not an invented GPS timestamp. Cloud
  receipt still updates last seen; the hub needs real time for its awake indicator.
  A 30-second receipt/command window follows presence, then the normal workflow.
  A full BOOT/user/GPS report follows when required and valid UTC is available.
  Home-only reporting wakes also confirm the scan with HOME_BEACON_SEEN; the
  initial presence is provisional and never claims a newly heard beacon.
- All personal profiles send presence on every scheduled reporting wake,
  including Power Save. Sleep intervals remain Normal 10 minutes, Power Save
  30 minutes and Active 1 minute; periodic Home GNSS refresh is unchanged.
  LED-only timer wakes and single-button feedback now send presence and listen
  for commands, but still skip GPS and preserve the regular reporting deadline.
  This personal policy overrides the shared Power Save check-in ratio without
  changing canonical V4 profile configuration.
- Normal, PowerSave, Active and Lost Alert use V4 timings. Home GNSS
  refreshes follow their profile ratios. Debug is rejected on this live target.
- Each telemetry send opens a 30-second receipt/command window. A missing receipt
  triggers one retry of identical bytes. A receipt does not close the window.
- Profile, status/ping and LED-find commands are supported through the current hub
  wire contract. Status/ping ACKs current state; it does not implement the future
  wake-on-radio/fresh-position Ping specification. Commands arrive at check-ins,
  not while the radio is asleep. No cellular fallback or collar Wi-Fi uplink.
- Short button wake requests a report; holding the button for two seconds at wake
  toggles Lost Alert/Active. Lost Alert keeps GNSS running and advertises the V4
  BLE find beacon, then returns to Active after two hours. A power reset with
  unknown time exits Lost Alert conservatively. Held buttons do not cause reboot loops.
- NVS stores profile and a 16-entry duplicate-command cache atomically before ACK.
  Repeated commands are ACKed without reapplying profile/LED effects. Sequence
  blocks are reserved before transmission to avoid reset reuse. RTC retains
  counters during deep sleep. Reflashing a different collar identity onto the
  same chip fails closed until its personal NVS namespace is deliberately reset.
- **Existing V4 limitation:** hub downlinks are unsigned. Source/destination and
  duplicate checks do not authenticate them. The hub sends time zero, so cloud/
  local command expiry remains the hub's responsibility; timestamped commands
  additionally receive collar age checks. Do not describe this as secure downlink.
- Hub-local and cloud command counters are independent. A recently used sequence
  with different contents is deliberately rejected by this collar. Prefer the
  cloud UI for normal use; if switching from local testing causes a collision,
  issue a fresh UI profile command to supersede it with a new sequence. Do not
  erase persistent state or weaken duplicate checks to work around the collision.
- Battery voltage and metre accuracy are unmeasured and encoded as zero; HDOP is
  used only to qualify a fix, not misrepresented as metre accuracy. The unchanged
  V4 UI may show a zero/empty battery reading. No battery-based automatic profile
  switching is claimed. Sleep current and battery life require measurement.

## Build and credentials

Run from the repository root. Use an installed PlatformIO executable (`pio` below).

```powershell
# Compile real source with hardware disabled; cannot be uploaded by this target.
pio run -d collar/personal-esp32s3 -e personal_compile_check

# After live registration, select exactly one collar's local credentials.
python collar/personal-esp32s3/provision.py --select 3001
pio run -d collar/personal-esp32s3 -e personal_collar
```

The ordinary live build refuses to compile without `.secrets/personal_config.h`.
It also refuses to transmit with an all-zero key. The compile-check image halts
before any hardware initialization, even if somebody manually flashes its binary.

`provision.py` uses the repository's existing cryptographic credential generator.
Its private SQL uses **inserts only**, with collision checks in one transaction;
it never rotates credentials belonging to another registration. Generation alone
does not register anything. Apply SQL only to the verified live backend.

```powershell
# Fresh installations only; never rerun over an existing bundle.
python collar/personal-esp32s3/provision.py --household YOUR_FAMILY_UUID --endpoint https://YOUR_PROJECT.supabase.co/functions/v1/ingest-position --ssid "Reesnet Guest"
```

Defaults are collars **3001, 3002, 3003, 3004**, hub **0030** (48 decimal).
After registration they can be renamed in the existing web GUI. The credentials,
SQL, backups, binaries and serial logs are private local files, excluded from Git.
Keep a secure backup of `.secrets` outside this disposable worktree before archiving it.
Do not share firmware binaries publicly: they contain device/gateway credentials.

## Flashing and rollback

1. Verify the physical port/chip identity. First collar: COM4, ESP32-S3,
   MAC `d8:3b:da:75:ef:2c`, previous V3 ID 698. Monitor at 115200 baud.
2. Save a full 8 MB flash image and SHA256 before replacing firmware. It includes
   old NVS and filesystem data. Store it only under `.secrets/backups`.
3. Select the correct identity, build, and upload:

```powershell
pio run -d collar/personal-esp32s3 -e personal_collar -t upload --upload-port COM4
```

If the sleeping device does not connect, hold BOOT, tap RESET, then release BOOT.
Check for a changed COM port. Never erase flash merely to make upload work.

Rollback uses esptool with the known device port:

```powershell
python -m esptool --chip esp32s3 --port COM4 write_flash 0 .secrets/backups/collar-698-before-uplift.bin
```

The example backup path is relative to this firmware directory. Confirm the file
is exactly 8,388,608 bytes and its SHA256 matches the saved manifest. A full restore
also restores previous identity/settings. Do not restore one collar's backup to another.

## Verification

Native C++ tests in `tests/policy_test.cpp` exercise real shared headers and the
new policy; `tests/verify_fixture.py <test-executable>` independently verifies the
generated packet with Python HMAC and struct decoding. The fixture is public
test data and is never posted to the live backend.

Before outdoor reliance, record actual results for:

- Real GNSS location/UTC outdoors, indoor no-fix and stale-fix recovery.
- LoRa receipt followed by a separate command; wrong recipient, duplicate and
  expired commands; profile persistence after deep sleep and power reset.
- Hub relay accepted by the existing backend and real position on the Vercel map.
- BLE home/away transitions, all profiles, button behaviour and lost timeout.
- Radio outage/recovery, hub Wi-Fi outage/off-grid operation and reconnection.
- Battery-powered sleep and active current, a 24-hour soak, range/recovery walk.

A successful build or serial boot is not a substitute for these hardware checks.
See `VALIDATION.md` for this rollout's recorded results and outstanding work.

For a bounded serial soak (PlatformIO Python includes pyserial):

```powershell
python collar/personal-esp32s3/monitor.py --ports COM4 COM17 --seconds 86400 --output collar/personal-esp32s3/.secrets/soak.jsonl
```

Do not run another serial monitor/uploader on those ports simultaneously. Keep
the collar powered continuously; repeated USB resets invalidate a sleep/soak run.
The logger reopens native USB when the collar wakes from deep sleep. Its summary
counts serial-unavailable events separately; those are not automatically crashes.

## Replacement hardware with an existing identity

Before reusing an identity on a new board, preserve both boards' recovery images
and check the highest live report number. A private `PERSONAL_SEQUENCE_START`
override can seed a fresh board above that number; its persisted counter then
takes precedence. The default remains 1 for newly registered identities.
Keep the failed board disconnected. Never copy another collar's credentials.

## Optional external D1 LED

Define `PERSONAL_D1_LED 1` only in the fitted collar's private build header.
Wire GPIO2 (XIAO D1) through a 330-ohm to 1-kohm resistor to the LED anode;
connect the cathode to GND. GPIO HIGH lights it; LOW turns it off, including sleep.
Other collars leave GPIO2 untouched. Remove the previous FPV finder entirely.

The existing Find Alert Chirp command produces three 150ms flashes with 80ms
gaps. OFF stops the output; other buzzer patterns are unsupported. No sound is
generated. The GUI duration is not implemented. Duplicate commands re-ACK
without replay. Enabled assemblies use D1 for Find feedback instead of GPIO48;
other assemblies retain their previous GPIO48 Find behaviour.

USER gestures work both awake and when waking from sleep:

- Single short press: seven rapid D1 flashes (70ms on/70ms off, 910ms total).
  From sleep, this also sends the universal wake presence and opens command RX.
  It does not request GPS or telemetry. From sleep it resumes the remaining
  scheduled sleep when retained UTC is available.
- Double press: second tap within 350ms after the first release requests the
  GPS/report process. Existing GPS quality gates and the 60-second limit apply.
- Hold three seconds: toggle Lost/Active, persist the profile and request a report.
  Holding longer does not repeat the toggle; release is required for another action.

Inputs are debounced for 30ms. A background sampler captures gestures during
blocking radio/library calls; the main loop applies queued actions at its next
service point. BLE scanning is asynchronous so feedback remains serviced.
Single feedback waits for the double-tap window to close. Debug mode is not toggled.

While awake, serial `led flash` tests three flashes without GNSS time and `led off`
stops them. Timed flashes do not add delays to GPS/command handling. The receive
window completes a sequence already started; entering sleep cancels any remainder.

## Cloud LED schedule extension

The separately reviewed web/backend/hub LED feature uses CONFIG tags FA (action),
FB (duration, u16 seconds), FC (interval, u16 seconds). Flash now gives seven
rapid D1 flashes. Repeat defaults to ten minutes at a fixed minute interval;
Stop cancels it and suppresses automatic Lost flashes until Lost is re-entered.
The collar owns the timeout; leaving the browser does not stop the cycle.
Automatic Lost feedback also uses seven flashes per minute.

LED-only timer wakes preserve the telemetry deadline rather than requesting GPS
on each minute wake. Persisted UTC deadlines and sequence ordering prevent a
retry/reset from extending a repeat. Unfitted outputs reject the command.
Only 3004 has the new image installed so far. Other collars require individual
builds with PERSONAL_D1_LED enabled and their own existing credentials.

GUI commands still need a check-in for delivery; Stop/Flash now are not an
unsolicited wake. The web release and queue migration are separate from this
personal branch and remain pending review. Live end-to-end validation is pending.

The personal hub adapters share the same 30-second command timing override.
Profile sleep intervals and the two-second receipt ACK retry remain unchanged.
The deployed GUI currently shows a shorter conservative awake indication; this
firmware update alone does not extend that indication. Longer RX windows add
awake time, including on LED-only wakes.
