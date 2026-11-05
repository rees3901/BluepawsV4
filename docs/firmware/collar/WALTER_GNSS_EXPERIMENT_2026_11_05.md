# Walter GNSS acquisition experiment — 5 November 2026

## Scope and fixture

Walter on COM10, device **1011**, existing bearer/HMAC/APN/CA retained. The user
confirmed a stationary patch antenna pointing up with clear sky and supplied
reference **51.905909, -2.239490**. This is a user-supplied point, not a surveyed
reference. All “error” below means horizontal separation from that point.
Neither the reference nor a fabricated position is fed into the modem or used
by firmware to select samples.

The original `a9102b9b8579` image, exact provisioning header, device registry,
and flash prefix including NVS were backed up privately on reesPC before
flashing. The read-back app matched the saved binary. Secrets are excluded from
Git and remain under the existing ID; no credentials were rotated. Recovery
files live in the ignored `.private-walter-backups/1011-gnss-20261105/` directory
of the canonical checkout. These files contain embedded credentials and are
restricted to the Windows user.

## Experiment definition

- Original image: three single-event runs, three old 60-second settling runs,
  three hot-after-first-valid runs. Old single logs lack coordinates, so they
  support uncertainty/availability observations but not reference-error statistics.
- Experimental source: `29fc1a1eab80`, pinned WalterModem 1.5.0 source
  `8debb2155ad4e1bfacd2063bbc00dd2256beedb6`; live modem release **UE8.2.1.0**.
- Selection refinement `9b6677cd0c1f`: after a settled group, select within its
  final three samples so an earlier isolated point cannot override that group.
  This build still selected the lowest-uncertainty fresh sample at the deadline;
  the later fallback refinement below addresses an observed failure.
  Additional cleanup timing and concise host output distinguish
  modem/RF cleanup from the sampling window and avoid blocking serial reads.
- Compare first-event snapshots with 15, 30 and 60 second repeated windows,
  plus a 60 second hot-start option. Repeat before/after assistance inspection.
- Provisional early completion: three successive fresh samples, uncertainty
  at most 25 m, increasing timestamps, and a 25 m position-step allowance plus
  bounded finite reported horizontal speed multiplied by elapsed sample time.
  Speed above 15 m/s is not used to expand the gate. These motion thresholds
  need moving-collar validation; stationary tests cannot establish that behaviour.
- Final policy: after a settled group, select its lowest-uncertainty original
  sample; at the deadline without a group, prefer the newest fresh sample meeting
  the 25 m target. If none meets the target, retain the lowest-uncertainty fresh
  coarse sample. Selection uses no reference position and does not ensure that
  the closest-to-reference sample is chosen.
- `SETTLED` means the experimental early-stop gate was met. `UNCONFIRMED` means
  a target-quality sample existed without three consistent samples; `COARSE`
  means only a poorer usable sample existed; `NONE` means no usable sample.
  Protocol validity and original uncertainty/age remain explicit. Labels are
  serial diagnostics, not new protocol fields or accuracy guarantees.
- The acquisition window is at most 60 seconds. RF-off/configuration, network UTC
  bootstrap, assistance download and cancellation confirmation are separate.
  The old firmware finished 60-second diagnostics about 0.2–0.3 seconds later
  because of cancellation/cleanup; this overhead is included in measured logs.

## Start conditions and time

“Cold/warm” is the modem’s configured acquisition mode, not a forced erase of
orbital data. True cold means no valid assistance; warm means valid assistance
without a known position; hot requires valid assistance and a known position
within about 100 km. An ESP reboot alone does not prove an unaided cold start.
The hot diagnostic uses a fresh real sample from the current session or a valid
anchor less than five minutes old. The exact `initial_hot` choice is logged.
No full modem firmware update or undocumented cache erasure is part of this test.

`ttf_ms` is the modem-reported time for each acquisition. Host arrival time and
whole-window duration are separate measurements. A fix’s timestamp is never
rewritten to its arrival/selection time. Rejection logs identify host/fix UTC
and the failed rule rather than guessing that every rejected event is poor RF.

## Reproduction

```powershell
pio run -e walter
node tools/test_walter_testbed.mjs
python tools/test_walter_gnss_tools.py
python tools/walter_gnss_run.py --out .pio/comparison.jsonl --commands "gnss single" "gnss window 15" "gnss window 30" "gnss window 60" "gnss hot 60" --repeat 3 --resume
python tools/walter_gnss_analyse.py .pio/comparison.jsonl --reference 51.905909 -2.239490 --out .pio/comparison-results
```

Stop/Idle sequencing is enforced. The runner opens USB with DTR already low to
avoid deliberately rebooting the ESP. `--startup` captures an automatic boot
cycle first. `--seed-clock` explicitly uses PC UTC for a diagnostic if cellular
time is unavailable; it supplies no coordinates. Leave the antenna unmoved
through comparisons. Rotate test order across repeated series to reduce order
bias. Results remain a small uncontrolled outdoor comparison with changing
satellite geometry, not a randomised certification test.

## Production implications

The final GM02SP must supply a common timestamped GNSS solution to both LoRa
and LTE paths. A bounded sampling policy belongs before packet construction;
LTE fallback must preserve the same packet/sample semantics. The Walter board
has no physical SX1262, so only the packet/fallback policy can be tested here.
See [the production runtime decision](COLLAR_RUNTIME_DECISIONS.md#gm02sp-acquisition-quality-before-either-uplink).

Reference error, reported uncertainty and sample spread are distinct. No USB
current meter or supply-voltage trace is available in this test, so acquisition
milliseconds are a duty-cycle measure, **not measured energy**. Validate moving
tracks, body/antenna orientation, supply bursts and energy per acquisition on
production hardware before choosing the final profile budgets.

## Vendor sources

- [QuickSpot GNSS API](https://github.com/QuickSpot/walter-documentation/blob/main/walter-modem/arduino_esp-idf/reference/gnss.md): single-fix API, high sensitivity default, acquisition modes, UTC hint and assistance API.
- [QuickSpot GNSS FAQ](https://github.com/QuickSpot/walter-documentation/blob/main/faq/gnss-gps.md): passive antenna support and start-condition distinctions.
- [BluePaws vendor reference](WALTER_GM02SP_UPSTREAM_REFERENCE.md): board/bare-modem distinctions and sequencing constraints.

## Measured comparison on the second build

Four repeats per mode on `c8d5e3a5b686` (code refinement `9b6677cd0c1f`),
with test order reversed relative to the initial series:

| Request | Selected / runs | Settled | Median first modem fix | Median window | Median reference separation | Worst separation |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Single fix | 4/4 | 0 | 3.11 s | 3.79 s | 7.40 m | 22.42 m |
| 15 s cold/warm | 4/4 | 3 | 3.06 s | 13.65 s | 2.29 m | 4.91 m |
| 30 s cold/warm | 4/4 | 3 | 3.01 s | 15.62 s | 9.55 m | 20.75 m |
| 60 s cold/warm | 4/4 | 4 | 3.15 s | 16.08 s | 8.12 m | 10.72 m |
| 60 s hot option | 4/4 | 3 | 5.48 s | 25.18 s | 10.71 m | 28.21 m |

The original-image series had median selected separation about 29 m for both
settling and hot-after-valid modes, with one 74.82 m result. The first experimental
series then had better reception; a reboot/modem restart and changing satellite
conditions separate these phases. **Do not attribute that whole difference to
the code.** Four stationary runs cannot establish the optimal production budget.

Assistance inspection returned available almanac (9,660,813 seconds remaining)
and real-time ephemeris (9,405 seconds to expiry, 5,805 seconds until update).
`assist` found no refresh due, performed no download and returned READY. This
was a cache check, not an assisted/unassisted A/B experiment. No true unaided
cold start was established. That test still needs a vendor-supported cache-clear
procedure or a fixture with proven empty cache.

The first experimental automatic boot spent 185.98 seconds trying to obtain
LTE/time, then returned `Network/time bootstrap failed` with no GNSS attempt.
Subsequent diagnostic comparisons explicitly used PC UTC. That seed supplies no
coordinates and does not demonstrate standalone UTC recovery without LTE.

## Stress checks and selection refinement

- Five stop requests about one second into acquisition returned NONE without
  fabricating coordinates; CLOSED occurred after 1.072–1.279 seconds, with every
  RF-off cleanup confirmed in 19 ms. Subsequent acquisition recovered.
- Three 5 second windows returned after 5.239–5.246 seconds including cancellation
  confirmation; all RF-off cleanups were 19 ms. Only one of three selected points
  was within 25 m of the reference. One sample estimated 7.2 m uncertainty but
  was 34.54 m from the reference. A quick answer is not necessarily a good one.
- Repeated diagnostics on the second build kept free heap at 299,036 bytes and
  uptime increased without an unexpected reboot. This does not measure USB
  voltage or exclude supply noise; no rail/current trace was taken.
- One first-build diagnostic showed a 41-second gap between selection and the
  host observing Idle. Later runs use concise stdout while continuously draining
  serial to disk, and explicit device RF cleanup timing. Do not interpret that
  earlier host gap as GNSS time to fix or claim its cause is proven.
- A follow-up `send` window on the second build ended without a settled group
  and chose an earlier 5.0 m-uncertainty sample, 38.16 m from the reference. A
  newer 18.0 m-uncertainty sample was 16.68 m away. The final fallback therefore
  prefers the **newest fresh target-quality sample**. Both timestamps remain
  unchanged. A regression test covers the selection rule without a reference.
- Offline replay also compares a central original point (a medoid) within
  settled groups. Results were mixed across modes. That replay is not deployed
  as smoothing and does not justify reporting a smaller accuracy number.

No future-UTC rejection was reproduced in the PC-seeded comparisons. The earlier
10.5 m rejection remains unclassified; the new logs will identify the failed rule
if it recurs. Timestamp checks were not relaxed on the strength of a guess.

## Decision and retained evidence

Keep adaptive sampling with a **60 second ceiling**, early completion after
three qualifying samples, and ordinary cold/warm requests as the default.
The observed retained-state first fixes generally took about 3 seconds; hot
requests were slower here. There is no evidence to extend sampling beyond a
minute or force hot mode on every acquisition. Retain shorter-window diagnostics.

This improves bounded acquisition/selection behaviour; it does not prove 25 m
actual accuracy in all conditions. Consistent multipath bias can survive several
samples. Moving-collar tests, antenna-placement comparisons and measured energy
remain requirements before production release. The stationary reference cannot
validate the reported-speed allowance used by the consistency gate.

Machine-readable evidence: [summary](experiments/2026-11-05-walter/summary.json),
[trial/sample details](experiments/2026-11-05-walter/results.json), and
[every position sample](experiments/2026-11-05-walter/samples.csv). Raw serial
JSONL files are retained privately beside the recovery files. These normalised
results contain no bearer token, HMAC key, TLS private key or SIM identifier.

For future flashing, keep the private provisioning header and current NVS
sequence reservations. The full flash backup includes an older counter; normal
application rollback should retain current NVS rather than replay that old state.
