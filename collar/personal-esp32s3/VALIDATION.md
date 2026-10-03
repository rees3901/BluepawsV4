# Personal rollout validation — 3 October 2026

Base: canonical `origin/main` at `4c15f2983fb28786f4ac89af5c38f86f32c597e6`.
Branch: `codex/personal-legacy-collars`; no merge or production deployment.

## Provisioning

- Live Vercel project: `bluepaws-v4-web`; production alias
  `https://bluepaws-v4-web.vercel.app`.
- Backend independently verified from the deployed JS assets:
  `ykcdaonkvwemedotdpdr.supabase.co`.
- User selected the currently accessible Family, `Bluepaws Test Household`, for
  these real personal devices. No Family ownership or membership changes.
- New collars: **3001–3004**, displayed as `Collar 3001` through `Collar 3004`.
  Distinct HMAC and device credentials are registered; no existing keys rotated.
- New personal gateway: **0030** (48 decimal), separate gateway credential.
- The short-lived unused 2001–2004 registrations created earlier in this session
  were removed, including their credential/key records, after the requested
  change to the 3000 range. They had no reports or commands.
- Wi-Fi: open `Reesnet Guest`. Secrets remain in ignored local directories.

## Completed software checks

- Live collar PlatformIO build passes (ESP32-S3, RadioLib 7.1.2).
- Hardware-disabled compile-check target builds successfully.
- Personal Heltec build and LittleFS build pass; canonical hub source/assets
  are used unchanged.
- Native MSVC C++ policy tests pass: packet bounds, routing, malformed TLVs,
  profile validation, timestamp freshness, duplicate persistence, receipt ACKs.
- Independent Python HMAC/struct verification of the native wire fixture passes.
- Four provisioning tests pass: distinct material, identity validation,
  fail-on-conflict SQL, private-file overwrite refusal.

## Hardware evidence so far

- COM4 identifies ESP32-S3 MAC `d8:3b:da:75:ef:2c`, running reference legacy
  firmware `1b2ef63`, previous collar ID 698 before uplift.
- Full 8 MB collar backup saved; SHA256
  `84dee3c3a4e91bced1519f9436c1573e1ed45b3226f449398d2bad2b03133c4f`.
- Personal collar 3001 flashed on COM4; esptool verified flash contents.
- Original firmware images and credentials are additionally preserved outside
  the worktree at `C:\Users\reesPC\.bluepaws-personal\legacy-collars-20261003`,
  with directory access restricted to the current Windows user and SYSTEM.
- COM17 identifies ESP32-S3 MAC `44:1b:f6:f8:ec:bc`.
- Full 8 MB hub backup saved; SHA256
  `8eda68065944870008beced25b35f47b61e9989214d7dd9cf8b399c34c829095`.
- Hub bootloader, partition table, firmware and LittleFS flashed together;
  esptool verified each flash write.
- Previous hub NVS settings revision 24 prevented new identity revision 2 from
  applying. Reset the backed-up NVS partition only (`0x9000`, length `0x5000`).
  Live backend now confirms applied revision 2, BLE enabled and advertising.
- Hub connects to `Reesnet Guest`, IP `192.168.0.67`, fetches the Family snapshot,
  and appears as `Personal Heltec Home Hub` in the live Vercel UI.
- Local LAN HTTP request timed out from this host; guest-network isolation has
  not been diagnosed. Cloud connectivity works. Hub NTP synchronized at 20:52 UTC.
- Collar 3001's first boot acquired genuine GNSS UTC without a usable position
  (0 satellites, HDOP 25.50). The hub received its signed reports; the live backend
  accepted observations 7552 and 7553 at 20:52 UTC without updating the map position.
- The hub delivered an Active profile command, sequence 1. Collar applied it,
  ACKed it (hub RTT 726 ms), and entered 60-second sleep. Its next timer wake
  retained Active mode and initialized the radio successfully.
- Native USB disappears during collar deep sleep; the monitoring script now
  reconnects automatically without resetting the collar. Serial unavailability
  events alone do not imply a firmware crash.
- Bench testing exposed deferred first-fix acquisition while Home was detected.
  Personal firmware now retries GNSS each wake until a real first position exists.
  It was rebuilt and reflashed; persisted Active mode survived the reset.
- Outdoor acquisition: six satellites, HDOP 1.50, valid UTC and a stabilized fix.
  Observation 7555, sequence 65, recorded at 20:57:27 UTC and received at
  20:57:31.755 UTC through `lora_hub`; `position_updated=true`.
- The live Vercel map displays Collar 3001 at its real position. The next Home
  check-in preserved the fix and successfully relayed again after timer wake.
- Normal profile sent from the live GUI was applied and ACKed over LoRa. Cloud
  sequence 2 is `acked` at 20:59:00 UTC; UI confirms Normal and acknowledgement.
  Collar then entered the expected 600-second Normal sleep.
  The first UI command (sequence 1) was superseded before delivery because it
  overlapped the earlier local test sequence. Local/cloud counters are independent;
  the collar's conflicting-sequence rejection remains intact.
- Hub logging shows occasional spurious receive/structure errors immediately
  after its own transmissions, followed by successful real receptions/ACKs.
  This is recorded as a canonical hub observation; hub sources were not changed.
- No hub reboot/brownout message was observed during this bench capture. The user
  reported red indicator flashing while USB-only and is connecting a battery.
  The exact indicator cause has not been established. The display is intentionally
  not initialized by this unchanged browser-oriented hub firmware.
- A bounded 24-hour COM4/COM17 observation started at 21:01 UTC, 3 October.
  Logs and run metadata are in the private recovery directory (`soak-24h.jsonl`,
  `soak-run.json` and eventual summary). A follow-up review is scheduled for
  4 October. This is an in-progress USB-powered continuity observation, not a
  passed soak or battery-life measurement. Keep the PC awake and devices powered.
- That initial observation caught a real cadence defect: after the user disabled
  the hub beacon, the collar woke at 21:09:13 UTC, logged `seen=0 home=1 missed=1`,
  and returned to 600-second sleep without reporting. Wake-up itself succeeded.
  The initial soak is marked interrupted, not passed. Personal firmware now bases
  Home cadence on retained Home state (including the first missed scan), rather
  than requiring a fresh beacon detection. It still clears the fresh-beacon flag
  and switches Away after the second miss. Canonical sources remain unchanged.
- Correction `f728ca1` built and flashed successfully, with write hashes verified.
  With the hub beacon still disabled, report 129 acquired a real fix at 21:16:07
  UTC and reached the backend at 21:16:10.596 UTC (`position_updated=true`,
  Normal profile, Away status, GNSS-valid flag). A replacement 24-hour observation
  began at 21:17 UTC; its private files use the `soak-v2-` prefix. The transition
  fix still needs a complete Home-to-away regression cycle during qualification.

## Personal hub display upgrade — 3 October 2026

- User requested an on-device UI after initial browser-only rollout. The personal
  adapter adds eight ST7735 pages and USER-button navigation while compiling
  canonical hub services and protocol utilities unchanged. No canonical hub file
  or browser asset was edited. Display FSPI is separate from LoRa HSPI.
- Native button tests passed for bounce, tap, hold, no repeat/release action after
  hold, and millisecond timer rollover. Pinned PlatformIO build passed: static RAM
  74,988 bytes (22.9%); firmware 1,687,861 bytes (50.5% of application partition).
- Firmware uploaded to COM17 with verified write hash. Filesystem and NVS were
  preserved; live hub presence at 21:26:27 UTC confirms BLE remains disabled.
  The previous personal hub image is preserved as
  `hub/personal-0030-before-display.bin` in the private recovery bundle.
- With the display firmware running, collar 3001 woke on schedule and transmitted
  report 130. Hub received it at 21:26:37 UTC, acknowledged it, and the live backend
  accepted observation 7559 at 21:26:39 UTC (HTTP 201). This wake had no fresh GNSS
  fix, so `position_updated=false` and the previous real position remains in place.
- The v2 observation was deliberately interrupted for the hub firmware update.
  Replacement logs and metadata use `soak-v3-`; the scheduled review now points to
  that run. Neither interrupted run counts as a completed 24-hour soak.
- User confirmed the display and both USER button actions work. They also reported
  accidentally pressing RESET. The observed restart at approximately 21:26:46 UTC
  is therefore an intentional hardware action, not an unexplained crash. Serial
  confirms display initialization and Wi-Fi reconnection; live hub presence was
  accepted at 21:27:02 UTC with BLE still disabled. The v3 run metadata records this
  event; observation duration must not be confused with uninterrupted hub uptime.

## GNSS recovery correction — 3 October 2026

- COM4 showed repeated 15-second warm acquisition failures, including five used
  satellites at HDOP 5.4. The historical collar fix caused subsequent attempts to
  use the short warm timeout, regardless of receiver state or prior failures.
- Every personal GPS attempt now has one 60-second total limit, including wake
  and ten-second continuous stabilization. Fresh position, >=4 satellites and
  HDOP <=5 remain required. Quality loss resets stabilization, never the deadline.
  An exhausted deadline still reports failure without presenting stale data as valid.
- New checksum-validated NMEA diagnostics expose GGA/GSA/RMC/GSV/TXT and elapsed
  acquisition timing without dumping coordinates. They do not assert a receiver
  cold/hot start or a failed backup battery. Firmware sends no cold-reset command.
  The Seeed schematic includes an MS621FE backup cell on V_BCKP; its voltage and
  retention have not been measured. GPIO standby does not switch the main supply.
- Native tests passed recovery beyond 15 seconds, interrupted stabilization,
  strict timeout including late fixes, timer rollover, NMEA checksums/empty fields
  and diagnostics. The live build passed and COM4 flash write hash was verified.
- First acquisition: usable fix after 16,381 ms; stabilized after 26,381 ms,
  eight satellites, HDOP 3.2. Report 193 was accepted via LoRa hub at 22:10:46 UTC
  as observation 7581, with `position_updated=true`.
- Next timer wake after standby: usable after 14,582 ms and stabilized after
  24,584 ms, eight satellites and HDOP 3.0. Report 194 was accepted at 22:12:36
  UTC as observation 7582 with `position_updated=true`; backend latest position
  confirms Active and GNSS-valid. The receiver reported `ANTENNA OK`; diagnostic
  NMEA checksum errors were zero. Initial GGA quality 0/GSA type 1 progressed to
  GGA quality 1/GSA type 3. Latest RMC was V while the combined GGA fix was valid;
  individual talker reports are diagnostic, not a new acceptance rule.
  Acquisition timings do not establish receiver reset type or backup-cell health.
- Prior personal firmware is preserved as
  `collar/personal-3001-before-GNSS-recovery.bin` in the private recovery bundle.
  The previous observation was interrupted for this correction. New observation
  files use `soak-v4-`; scheduled review points there. No completed soak is claimed.

## Outstanding qualification

- Full loss-of-power persistence, radio/network outage and reconnection tests.
- Complete physical profile/button/find-beacon, Home/away and lost-timeout checks.
- Remaining three collars flashed individually with their own identities.
- Measured power consumption, continuous 24-hour soak, and range/recovery walk.

Do not equate successful builds, registration or hub presence with validated
outdoor tracking. This file will be updated as actual hardware evidence arrives.
