# Personal firmware lessons for canonical V4

Audit: 14 October 2026. Compared canonical main `75be5aa` with personal
`5abe25e`. The personal branch is reference evidence, never a merge source.
The canonical collar target is the RAK4631 testbed using the RAK4630 module
family (nRF52840/SX1262). A successful testbed build is not production PCB,
real GNSS, RF, battery or end-to-end validation.

## Already incorporated in main

| Behaviour | Canonical delivery | Qualification |
| --- | --- | --- |
| Home check-in on every scheduled wake, including Power Save | PR 257 | Normal 600s, Power Save 1800s, Active 60s; Home GNSS refresh every 10 detections; Debug differs intentionally. |
| LED command UI, queue, hub relay and ACK feedback | PR 256 | This does **not** establish RAK collar support: canonical `handleReceivedCommand` still uses the older Find handler. |
| Fast LED repeats restricted to reported Lost profile | PR 261 | A UI constraint; firmware must independently enforce supported behaviour. |
| Estimated GNSS accuracy and capped map circles | PRs 262, 264 | RAK telemetry already estimates HDOP × 5; keep this distinct from a receiver-provided uncertainty measurement. |
| Separate fault bar and command lifecycle feedback | PRs 260, 265, 270–272 | Hiding generic Error did not fix firmware that still emits Error instead of Out. |
| Hub BLE advertising controls with capability gating | PRs 263, 269 | Presets are implemented for supported ESP32 hubs; do not copy their numeric controller enums to Nordic collars. |

## Extracted in this change

`collar/include/collar_report_policy.h` expresses location and report flags
without GPIO, RTOS, sleep, radio, modem or GNSS driver dependencies. Canonical
telemetry, boot and confirmed Home check-in paths use it.

- Home/Out is based on BLE evidence, independent of fix validity.
- Explicit Lost profile keeps Lost status even when Home is heard.
- At Home, failed/stale GNSS alone produces no GPS fault badge. Valid-fix bits
  remain truthful; unavailable coordinates are never labelled valid.
- RF, cellular and module errors remain visible at Home.
- Away/Lost retains GPS error and stale-fix evidence, without replacing location
  status with Error. Stale requires an actual prior fix.
- Presence-only packets omit GPS faults; this helper does not add an early wake
  packet or alter acquisition, scheduling, time, radio or power management.

The protocol enum and byte layout are unchanged. Existing older devices may
still send Error; this is a firmware correction, not a speculative frontend
reinterpretation of those old packets. A missing beacon means "not confirmed
Home", not proof that the cat physically left the house. Current name-based
BLE detection is not cryptographic authentication.

## Remaining transfers, in recommended order

| Work | Reuse | Required adaptation / gate |
| --- | --- | --- |
| Early presence before BLE/GNSS; 30s command opportunity | Existing draft PR 259 | Rebase and resolve command/profile transitions before merging. A newly received profile must restart relevant counters and determine the next path, including immediate Lost entry. Avoid accidentally stacking two full 30s idle windows. Test zero-time signed presence, ACK/retry, power cost and real RAK wake sources. |
| Periodic / one-shot / cancel LED commands | Personal pure schedule logic and existing cloud contract | Implement canonical command decoding, bounded validation, duplicate handling and ACK only after successful application. Choose RAK output and timer wake adapter; do not copy D1/GPIO2. Verify repeat while asleep and Lost limits. |
| Button gestures and nonblocking flash signatures | Pure gesture state machine and tests | Decide the canonical UX explicitly (personal: single feedback, double report, 3s Lost toggle). Nordic button wake/task synchronization and debounce require their own adapter. Avoid silently changing the current canonical short-press report UX. |
| Bounded GNSS quality/stabilisation | Acceptance policy, hard deadline and diagnostics approach | Keep receiver adapters separate. L76K NMEA/standby and backup-cell diagnostics do not apply to GM02SP. Review vendor source and installed modem firmware before changing acquisition or LTE/GNSS sequencing. Current canonical cold acquisition is 20s plus stabilisation, boot up to 60s; the comment describing 60s cold does not change the 20s constant. |
| Home marker placement and fixed hub fallback | User-facing requirement | Preserve raw GNSS history; display Home at a known associated hub position with its source identified. Check hub association and fallback provenance rather than overwriting a measured collar fix. Not implemented by the report-policy change. |
| BLE diagnostics | Counts, strongest RSSI, rejection reasons | Use Nordic scan callbacks and avoid third-party device identities/precise positions in logs. Reception is evidence of some RF operation, not an antenna-continuity test. |
| Durable state and time correctness | Sequence recovery, duplicate handling, independent last-seen and fix times | Audit Nordic flash wear/replay protection and reset recovery independently. Do not copy ESP RTC memory or fake UTC from build time. |

## Keep personal

XIAO ESP32-S3/B2B SX1262 pin mapping, GPIO2 LED, external finder/buzzer
experiments, L76K UART/standby/NMEA and backup-cell handling, ESP deep sleep and
USB-port behaviour, personal hub build adapters, generated source patches,
private provisioned identities/keys/Wi-Fi/home coordinates, and the branch-only
Vercel deployment block. Production cellular functionality must remain intact.
No personal firmware directory, private configuration or deployment block is
included in this extraction.

## Validation and release boundary

This change requires host report-policy tests and a canonical `pio run -e collar`
build. The host test covers all profiles, Home/Out/Lost precedence, valid/stale
fixes, non-GPS faults, presence flags and packet CRC/structure. It uses synthetic
identities and no real keys. It does not establish HMAC provisioning or real RF
acceptance. Run the existing routing and uplink ACK tests for regressions.

Before flashing canonical hardware: check Home/no-fix, Away/no-fix, Lost/Home,
independent faults at Home and signed cloud acceptance using a separately
provisioned RAK test device. Do not use the personal collar credentials or flash
an ESP32 image onto RAK hardware. Keep PR 259 draft until its runtime and power
checks are completed. Broader transfers should be separate reviewable PRs;
none warrants merging `codex/personal-legacy-collars` wholesale.

Host test on a machine with g++:

```sh
g++ -std=c++17 -Ishared/lib/BluepawsProtocol -Icollar/include collar/tests/report_policy_test.cpp -o report_policy_test
./report_policy_test
```

MSVC equivalent from a Developer Command Prompt (create the output directory first):

```bat
cl /nologo /EHsc /std:c++17 /Ishared/lib/BluepawsProtocol /Icollar/include collar/tests/report_policy_test.cpp /Fo.pio/report-policy-tests/report_policy.obj /Fe.pio/report-policy-tests/report_policy.exe
.pio\report-policy-tests\report_policy.exe
```

14 October extraction validation: MSVC host policy test passed; canonical RAK
PlatformIO build passed (16,724 bytes RAM, 183,616 bytes flash). Hardware, real
GM02SP operation and cloud acceptance were not tested in this extraction.
