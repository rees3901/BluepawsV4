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

## Not yet established

- Full loss-of-power persistence, radio/network outage and reconnection tests.
- Complete physical profile/button/find-beacon, Home/away and lost-timeout checks.
- Remaining three collars flashed individually with their own identities.
- Measured power consumption, continuous 24-hour soak, and range/recovery walk.

Do not equate successful builds, registration or hub presence with validated
outdoor tracking. This file will be updated as actual hardware evidence arrives.
