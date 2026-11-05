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
  Deadline fallback still selects the freshest eligible lowest-uncertainty
  original sample. Additional cleanup timing and concise host output distinguish
  modem/RF cleanup from the sampling window and avoid blocking serial reads.
- Compare first-event snapshots with 15, 30 and 60 second repeated windows,
  plus a 60 second hot-start option. Repeat before/after assistance inspection.
- Provisional early completion: three successive fresh samples, uncertainty
  at most 25 m, increasing timestamps, and a 25 m position-step allowance plus
  bounded finite reported horizontal speed multiplied by elapsed sample time.
  Speed above 15 m/s is not used to expand the gate. These motion thresholds
  need moving-collar validation; stationary tests cannot establish that behaviour.
- At the deadline select the fresh original sample with the smallest modem
  uncertainty; ties prefer the later sample. This is not selection by reference
  error and does not ensure that the closest-to-reference sample is chosen.
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
