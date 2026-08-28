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
