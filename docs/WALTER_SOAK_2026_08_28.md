# Walter 1010 repeatability soak — 28 August 2026

## Scope and controls

Started at **22:42:43 UTC** from `codex/walter-repeatability-soak`, based on
`main` commit `da33e61`. Walter alone was flashed on COM26. COM7/23/11 were not
opened or changed; device 1001 was still reporting through the existing setup.
Nine pre-existing PCB/tool files matched their saved hashes after this work.

The firmware uses the real shared profile schedule, real GNSS, real LTE and real
signed packets. LoRa TX completion is explicitly simulated, with no receiver or
invented hub ACK. The modem is put RF-off between cycles; the ESP waits in
FreeRTOS, not deep sleep. This is not a battery-life test. UTC is explicitly
seeded from the host; no position is seeded. GNSS settling diagnostics have not
been silently substituted for the normal acquisition policy.

Private evidence: `.pio/walter-soak-20260828/serial.log`, `state.json`,
`packet-checks.json`, `stdout.log`, `stderr.log`. The persistent monitor owns
COM26/115200 and polls local status every 30 seconds. To stop the **collar**:

```powershell
& "$env:USERPROFILE/.platformio/penv/Scripts/python.exe" tools/walter_soak.py --run-dir .pio/walter-soak-20260828 --request stop
```

Wait for Idle; closing capture by itself does not stop scheduled reports. There
is no automatic reset/restart. Keep the host powered and awake for serial capture.

## Initial evidence

| UTC | Evidence | Result |
| --- | --- | --- |
| 22:42:43 | Normal/away online worker cycle 1 | Started without a forced `send` |
| 22:43:28 | GNSS event, 12 satellite entries, unusable uncertainty | Correctly rejected; no coordinates fabricated |
| 22:43:50 | Packet 2049, observation 6508, `cellular_direct`, Normal | Real cloud receipt; GNSS invalid |
| 22:43:50 | Cloud command sequence 5 applied | Profile Active |
| 22:43:52 | ACK packet 2050, observation 6509, ACK TLV 5 | Cloud command row `acked`, attempts 1 |
| 22:43:50–22:44:01 | LTE command window | About 11.36 seconds, including authenticated polls |
| 22:44:02 | Worker sleep | Active, 60 seconds, RF off |

Captured packet 2049 passed independent HMAC decoding using the existing local
device key. Its SHA-256 matched observation 6508:
`982a8fb5c2dd37cfaebf4e7687f4820f348e6d013f3b8ab3b1343964c24897bf`.
ACK packet 2050 matched the serial/cloud hash
`d6dcd7d71b46dd6efa9c92919666bcaa8e3a0424a1658933ae2ff9834b288bf3`.
Command UUID: `b8feb161-d7c9-467a-bfb1-7c1f8dd57a81`. It was queued through the
authorized admin test path with `requested_by = null`, not a simulated user or a
manually edited ACK. No claim is made about testing the dashboard button here.

## Remaining live gates

### Clock defect found before the unattended run

Cycle 2 woke at 22:45:02 after the real 60-second wait and obtained a valid
56.5 m reported uncertainty at 22:45:53. Its sample timestamp was 22:45:03.
The ordinary acquisition handler incorrectly assigned that sample timestamp to
the current clock, moving UTC back by approximately 50 seconds and advertising
fix age zero. The monotonic sleep stayed correct, but UTC schedules, fix ages and
command expiry could become wrong over repeated cycles.

The run was explicitly stopped at **22:46:31 UTC**, Idle confirmed, capture closed
and Walter alone reflashed. Ordinary acquisition now uses the same freshness and
validity checks as the settling diagnostic and retains the established UTC
anchor. It does not relabel the sample time or accept an already stale/future
sample as a fresh fix. It requires an established UTC source, consistent with
the existing host seed/network bootstrap flow.

Added actual-function regression cases for a 50-second acquisition retaining
real fix age, rejection at 60 seconds, future timestamps, missing clock,
cancellation and timeout. The host suite and firmware build passed; new flash
size is 436,057 bytes, RAM unchanged at 64,272 bytes. Capture restarts in the same
append-only serial log; `state.json` describes the latest monitor session.
Active command 6 (`1743326d-a57c-4bdf-9ca2-dd3420d8be18`) was queued at 22:48:01
for the restarted Normal BOOT check-in. This is a new baseline, not uninterrupted
uptime or a completed recovery test.

### Gates after the clock fix

Restarted at **22:48:11 UTC**. At 22:48:55 the real GNSS result reported 93.3 m
uncertainty with sample UTC 22:48:15. Packet 2305 preserved fix age **39 seconds**
and current packet UTC, rather than age zero. It passed independent local HMAC
verification; its hash matched Supabase observation **6514**, received at
22:49:14 through `cellular_direct`, GNSS valid, accuracy rounded up to 94 m:
`09ffcdb5d205896a12f956e14ae44ae2b305c7188aed8469f4df7e4e8a213ce3`.

Command 6 changed Normal to Active. ACK packet **2306**, observation **6515**,
carried ACK TLV 6 and Active profile; the command row was `acked` at
22:49:16.517659 UTC, attempts 1. Its serial/cloud hash was
`559b4d56dbb5f64db6fa4fa96ead033a305f45fea612937547767c2526c3f2de`.
The LTE window ran 22:49:14.605–22:49:25.873 (11.27 seconds). Sleep began
22:49:26.744 for 60 seconds, with the UTC next-wake now matching the host clock.
Status at 22:49:42 showed online, running, Active, free heap 298,964 bytes.
Device 1001's cloud last-seen was still current at 22:49:20.

At the 22:49 checkpoint this verified one real GNSS/BOOT LTE upload after the
clock fix, not the full recurring baseline. Normal was not yet queued then.
The follow-up should queue it near the Active cycle-5 LTE opportunity, with no
forced `send`. Keep this PR draft until the remaining evidence is established.

1. Observe unforced Active wake cycles and its scheduled LTE attempt at cycle 5.
2. Queue Normal near that delivery opportunity; verify a signed cloud ACK and
   the subsequent 600-second wait.
3. Observe multiple genuine GNSS-bearing scheduled LTE uploads, including Normal.
   Normal/away uses every tenth cycle; a cycle includes acquisition and transport
   time in addition to the ten-minute wait. Quiet LTE periods are expected.
4. Only then begin a separately recorded, bounded hardware recovery experiment.
   Do not disturb the hub/1001, SIM service, credentials or antenna to create it.

The initial GNSS failure means the sunny-day baseline has **not** yet passed.
No live faults have been injected. Build and offline tests passed: Walter
PlatformIO build, actual firmware host contract/policy suite (including profile
wait/cancellation and malformed/expired/duplicate command cases), four LTE poll
tests and two soak evidence-parser tests. Offline recovery tests are not hardware
recovery evidence.

Heartbeat automation `walter-1010-repeatability-soak` checks the capture/cloud
every ten minutes. It is instructed to report and pause at **2026-08-29 10:45 UTC**
after approximately twelve hours of observation, leaving the board/capture
running. It must not conceal failures, shorten profile cadence or duplicate an
already pending Normal command. Milestones below should distinguish observation
from inference and must not declare completion before the remaining gates pass.

## Command routing decision

Use a single queue for each device and return a pending command via the path of
its authenticated check-in. No unsolicited LTE wake-up and no speculative
parallel transport fan-out. The live claim function accepts both paths and
retains command identity for redelivery until ACK/expiry. A future dual-radio
collar needs shared duplicate handling across both receivers; the separate boards
do not validate that integration. Current dashboard commands expire after ten
minutes, which can be shorter than a Normal LTE interval; that expiry must remain
visible, not be misreported as delivery or silently extended.

## Follow-up — 23:01 UTC

The monitor remains running on COM26; no second serial connection, forced report,
reset, firmware change or live fault injection was made during this check.
Six completed Active sleep intervals measured **60.013–60.015 seconds** from
logged sleep to next start. Cycle 7 finished at 23:01:19 and selected another
60-second wait. Free heap between operations remained 298,964 bytes; latest
status showed running/online Active, uptime 811 seconds, with no reboot observed.

The first **unforced scheduled LTE upload**, cycle 5, packet **2310**, reached
Supabase as observation **6524** at **22:57:14.400258 UTC**, Active profile,
`cellular_direct`. The serial packet passed independent HMAC verification and
matched the cloud SHA-256:
`0389fa204ddda23ada1efe17389f3ed7c8bfab7aa7f7d1ac0f6d131dd32f5eaf`.
The receive window ran 22:57:14.456–22:57:24.796, **10.34 seconds**. No command
was queued in that window, so there was no Normal ACK to expect yet.

GNSS remains inconsistent. Cycles 2/3/4 reported valid uncertainty values of
359/324/84 m respectively. Cycles 5 and 6 returned unusable 20,000,000 m values
and were rejected. The cycle-5 cloud observation correctly has `gnss_valid=false`,
no valid coordinates, accuracy zero and previous-fix age 162 seconds. Cycle 7
recovered a valid result with 168 m reported uncertainty. These are receiver
uncertainty estimates, not measured position errors. Nine captured packets across
both sessions passed HMAC verification; non-LTE stub packets are not claimed as
cloud deliveries. Device 1001 still had a recent cloud last-seen at 23:00:12.

After verifying no pending/sent unexpired command, queued **one Normal command**
at **23:01:49.502219 UTC** through the existing admin test path (`requested_by`
null): sequence **7**, UUID `a2e5a13b-d2b0-41ca-b2a5-7bf2f64cb23a`, expiry
**23:11:49.502219 UTC**. It is pending, not yet applied or acknowledged. The next
natural LTE opportunity is Active cycle 10 (roughly 23:07–23:08 at the observed
cadence; not guaranteed if acquisition/registration takes longer). Do not enqueue
a duplicate, force a report or silently extend expiry. Check command 7 and the
following 600-second sleep at the next heartbeat.

Transport cadence and scheduled LTE delivery have passed this checkpoint;
Normal restoration and multiple GNSS-bearing scheduled cloud reports have not.
The full sunny-day baseline remains incomplete, PR #147 stays draft, and live
recovery/fault injection stays gated. Capture and scheduled checks continue.

## Follow-up — 23:13 UTC: Normal restored

The next natural LTE opportunity arrived earlier than estimated, because GNSS
acquisition was quicker. Active cycle 10 started at 23:05:29.392 UTC. Packet
**2315** was accepted as Supabase observation **6531** at **23:05:57.239974 UTC**,
`cellular_direct`, profile Active, **GNSS valid**, fix age **11 seconds**, encoded
uncertainty **105 m**. Independent local HMAC verification passed, and the
serial/cloud SHA-256 matched:
`e96072da8083bd960b755a75dc6eca6c1a17d778bffa38e070cb5102b64a5582`.
This is the first GNSS-bearing *scheduled* LTE upload after the clock fix; the
earlier GNSS-bearing packet 2305 was a BOOT upload. Twelve captured stub packets
across both sessions now pass HMAC verification.

Pending **command 7** was applied as **Normal** at 23:05:57.132 UTC. Signed ACK
packet **2316**, observation **6532**, carried Normal profile and ACK TLV 7; its
serial/cloud hash matched
`10f7f2a1a43ef6dd8d6ab984ae2770d83de087517a029d72a26e0acb4cd74f42`.
The command row is independently confirmed **acked**, attempts **1**, at
**23:05:58.956555 UTC**, before expiry. Active command 6 also remains acked.
No duplicate command was queued and no ACK status was manually changed.

The LTE window ran 23:05:57.132–23:06:08.548, **11.42 seconds**. At
**23:06:09.238 UTC**, the worker selected the **600-second Normal wait**, with
next-wake UTC **23:16:09**. The completed interval is not yet measured at this
checkpoint; verify the cycle-11 start next time. Subsequent Normal LTE is due
at session cycle 20, not immediately at cycle 11. Preserve that cadence.

Latest captured status remains online/running Normal with free heap 298,964 bytes
and no observed reset. Nine completed Active waits measured 60.013–60.015 seconds.
Device 1001's cloud last-seen is 23:02:59.667 UTC and its reported profile is now
PowerSave (code 0); this check did not change it or open its ports.

Normal → Active → Normal application and both cloud ACKs are now verified.
Multiple GNSS-bearing scheduled uploads, a completed Normal wait and a scheduled
Normal LTE report are still outstanding. No live fault injection; keep the full
baseline incomplete and PR #147 draft. Monitoring and capture continue unchanged.

## Follow-up — 23:24 UTC: first Normal wait measured

Cycle 11 started at **23:16:09.243611 UTC**, **600.005797 seconds** after the
Normal sleep record at 23:06:09.237814. This completes the first measured
600-second interval after command 7. GNSS returned a valid sample with 98.7 m
reported uncertainty (encoded 99 m); packet **2317** preserved fix age 39 seconds
and passed independent HMAC verification. Its SHA-256 is
`4943cb996ca838d2034ff1790e2c709beacc4b1d1e9d08551b1723af4fd1a8cc`.
It was a simulated LoRa transmission only, **not a cloud upload**: cycle 11 was
not due for LTE. The worker selected another 600-second wait at 23:16:59.226560,
with next wake 23:26:59 UTC.

Supabase still shows observation 6531 / packet 2315 and ACK observation 6532 /
packet 2316 as the latest 1010 reports, with the previously verified hashes.
Commands 6 and 7 remain acked, one attempt each. No duplicate command, forced
report or unexpected LTE upload was introduced. Thirteen captured stub packets
across both sessions pass HMAC checks. At 23:24:27, capture was current, online
Normal was running, uptime was 2,190 seconds and free heap remained 298,964 bytes;
stderr was empty. Device 1001's last-seen remains 23:02:59 in PowerSave; this
read-only control check does not establish its current serial/radio state.

The Normal wait gate is now verified. Multiple GNSS-bearing scheduled LTE
uploads, including one under Normal, remain outstanding; cycle 20 is the next
expected LTE opportunity. Leave capture and monitoring running without fault
injection, profile changes or hardware access.

## Follow-up — 00:39 UTC: future GNSS sample rejected

Eight Normal waits have now completed at 600.005–600.006 seconds. Cycle 18
started at 00:31:33.262 UTC, but the modem's GNSS event at 00:32:12 reported
timestamp epoch **1787964004**, approximately **472 seconds ahead** of the
established device clock at receipt, together with an unusable 20,000,000 m
uncertainty. The firmware rejected it, did not rewind or advance UTC, and emitted
packet **2324** without valid GNSS coordinates. This is live confirmation that
the post-fix timestamp/freshness gate contains an anomalous future modem sample;
it is not a successful GNSS fix and does not establish its root cause.

Packet 2324 passed independent HMAC verification; SHA-256
`775380bb429aeb05a3ef3b7f7668344c045bff95d5221f4eb43727a56e616425`.
It was simulated LoRa only and is not claimed as a cloud delivery. The LTE cadence
remained quiet as expected, both commands remain acked once, and device 1001
checked in at 00:33:52 in PowerSave. Walter remains online/running Normal with
stable free heap and no reset. Continue the baseline; do not treat this contained
bad sample as authorization to begin fault injection.

## Follow-up — 01:00 UTC: scheduled Normal LTE baseline

After nine completed Normal waits of 600.005–600.006 seconds, cycle 20 started
at **00:53:16.552 UTC**. GNSS returned a valid sample with encoded uncertainty
78 m. Packet **2326** was accepted as Supabase observation **6536** at
**00:54:15.360931 UTC**, `cellular_direct`, profile Normal, `gnss_valid=true`,
fix age 0 seconds. The serial packet independently passed HMAC verification and
its SHA-256 matched the cloud row:
`8a2861742392a511087f1679fbdb1361bb52543aa1219cced014de86095120c3`.

The LTE receive window ran 00:54:15.479–00:54:25.598 (**10.12 seconds**) and
correctly found no command. No duplicate observation, command or ACK was invented.
The worker returned to a 600-second Normal wait. Walter last-seen advanced to
00:54:15.349 UTC with profile Normal. Commands 6 and 7 remain acked once. The
latest read-only 1001 control last-seen was 00:33:52.853 UTC in PowerSave.

This supplies two genuine GNSS-bearing **scheduled** LTE reports after the clock
fix: Active cycle 10 / observation 6531 and Normal cycle 20 / observation 6536.
Normal → Active → Normal ACKs, nine Active one-minute waits, ten Normal ten-minute
waits, scheduled LTE cadence, receive windows and packet/cloud correlation have
all passed. The sunny-day **transport/scheduling gate is passed**. It does not
mean GNSS quality is consistently good: uncertainty has ranged widely, invalid
samples occurred, and only the rejection behavior—not a GNSS root cause—is proven.

Re-ran non-hardware recovery/fuzz coverage while COM26 remained exclusively owned
by the monitor: the actual Walter host contract/policy suite passed, all four LTE
poll tests passed, and both soak evidence-parser tests passed. Coverage includes
malformed, expired and duplicate commands, failed polls, cancellation, bounded
timeouts, credential/receipt gates and sequence reservation failures. These are
not live modem/network recovery evidence.

### Bounded live recovery plan—not started

Keep this uninterrupted soak unchanged through its planned 10:45 UTC summary.
A separate later operator-window experiment should be scoped to device 1010 and
record a pre-fault cloud/serial baseline, one precisely defined failure, expected
RF-off/error behavior, the removal time, and the next natural recovery receipt.
Do not disturb COM7/23/11, device 1001, cloud authentication, SIM service or the
physical antennas. With the current image there is no safe one-shot serial fault
hook; creating one requires reviewed firmware, tests and a reflash, all forbidden
during this soak. Therefore no live fault has been injected automatically. PR
#147 remains draft while the longer run continues, and recovery remains explicitly
unproven rather than inferred from offline fuzzing.

## Follow-up — 01:12 UTC: possible one-second freshness edge

Cycle 21 started at 01:04:26.087 UTC after the eleventh measured Normal wait
(600.005 seconds). The modem event arrived at 01:05:29.688 with status ready,
12 satellite entries, 70.7 m reported uncertainty and GNSS epoch **1787965530**.
From the established cycle clock and wall elapsed time, `utcNow()` at receipt was
approximately epoch **1787965529**. The event therefore violated the current
strict `now >= fix.timestamp` condition by about one second and was rejected.
The ordinary log does not expose latitude/longitude, so this observation cannot
prove that the timestamp check was the only failing predicate; it does prove that
the sample violated that predicate. Treat the cause as an evidence-backed
inference, not a unique diagnosis.

The safe outcome was correct: packet **2327** carried no valid coordinates and
did not alter the clock. It passed HMAC verification; SHA-256
`fcfc7e7d62da5874d409e04694bb7fd1cadebb571312bd5e5cd084a8e450dec6`.
It was LoRa-stub only and not a cloud delivery. Normal scheduling continued,
both commands remain acked once, and device 1001 checked in at 01:04:12.

This suggests a post-soak improvement: explicitly test and consider a very small
future-sample tolerance for integer-second clock quantization while still
preserving the original sample timestamp, reporting age zero, never moving UTC,
and continuing to reject the observed +472-second anomaly. Do **not** edit or
reflash firmware during this uninterrupted soak. A tolerance change needs actual-
function unit tests at the accepted boundary and just outside it, followed by a
separate hardware run; no claim is made that such a change is already correct.

Cycle 22 independently repeated the apparent one-second edge. It started at
01:15:39.822 UTC; the event arrived 79.995 seconds later with epoch 1787966220,
status ready, 12 entries and 66.5 m uncertainty, while elapsed whole seconds put
the anchored device clock at 1787966219. It was rejected without coordinates.
This repetition strengthens—but still does not uniquely prove—the quantization
inference because ordinary logs omit coordinates. Packet 2328 passed HMAC
verification (`69a667c9562549a4a0e47435037600068093f8d06ba631f41255ed030b34d008`),
was not due for LTE, and Normal scheduling continued. Twelve Normal waits now
measure 600.005–600.006 seconds. The post-soak boundary-test recommendation and
the prohibition on modifying the live soak remain unchanged.

Cycle 24 produced the strongest practical example of the same edge. It started
at 01:38:40.150 UTC; the event arrived 43.751 seconds later with status ready,
five entries, reported uncertainty **9.0 m** and epoch **1787967564**, while the
anchored whole-second clock was approximately 1787967563. The event was rejected
and packet **2330** omitted coordinates. Its HMAC passed; SHA-256
`ebc0be3591bd19cd222f97a4d795ec09a95b32e1ba463b0977b3fde192b680bf`.
It was not due for LTE and did not reach Supabase.

This third occurrence makes the one-second-boundary hypothesis substantially
more credible and shows that the current predicate can discard a sample with a
good modem-reported uncertainty. It still does not prove actual 9 m positional
error or establish that all hidden coordinates were valid, because ordinary logs
do not expose them. Prioritize a tightly bounded tolerance test after the soak:
accept only the chosen integer-second boundary, keep the original sample time,
emit age zero without moving UTC, and retain rejection immediately outside the
boundary and for the observed +472-second anomaly. Fourteen Normal waits remain
at 600.005–600.006 seconds; Walter is otherwise stable and unmodified.
