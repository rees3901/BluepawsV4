# Collar feedback: cloud and local Home Hub

## Presentation

- Show a small light bulb with the remaining seconds of an expected ten-second receive window. It means **recently heard**, not a guaranteed connection or successful command delivery.
- Below the expanded card's command controls show a gently fading pending line, then acknowledged, failed, replaced or expired feedback. Respect reduced-motion preferences.
- New dashboard commands expire ten minutes after submission. Feedback disappears fifteen minutes after submission. Existing cloud commands retain their actual recorded expiry; other database callers still have the legacy default unless they explicitly request ten minutes.
- Reported power profile changes only when telemetry confirms it. Queueing a command must not optimistically relabel the collar.
- Lost status and Emergency Lost/Lost Alert power profile are separate from hardware errors. A real `ERROR_PRESENT` header flag remains visible, even in Lost Alert.
- The badge now includes a concise same-report reason; see [Collar fault summaries](COLLAR_FAULT_REASONS.md) for supported flags, limitations and its separate rollout.

## Separate sources, shared meaning

| Information | Local Home Hub | Vercel dashboard |
| --- | --- | --- |
| Receive opportunity | Monotonic time since live LoRa reception | Read-only Family RPC, conservative original receipt timestamps |
| Command state | Bounded RAM queue, serial/web/cloud submissions and matching radio ACK | Authenticated `device_commands` rows; private Family events trigger a fresh RLS read |
| Fault indication | Header `ERROR_PRESENT` | Latest accepted observation's flags |
| Verification | Newly received packets may still await cloud HMAC verification | Accepted cloud records |

No raw TLV, keys or command claiming is added to the cloud feedback RPC. Family permissions continue to apply to every source table. Revocation or failed reads clear supplemental feedback instead of preserving a potentially obsolete ACK/fault state.

Local journal restore does not start a receive timer. Appearance edits and same-record snapshots cannot extend a timer. Cloud replay, old packets, duplicate paths and missing original LoRa clock data cannot manufacture a fresh receive window. Network latency is subtracted conservatively; the cloud bulb may appear for fewer than ten seconds or not appear at all. Neither display is a transport guarantee.

The reset-reason TLV (`0x06`) is boot diagnostic information, not a fault code. The hub now exposes it separately as `resetReason`. Normal collar reports no longer put internal runtime errors in that TLV; the existing header error flag still reports a genuine fault. This is a local parsing/firmware correction, not a new cloud TLV interpretation or wire-format change.

## Command lifetime

The hub retains at most sixteen command feedback records in RAM. Active commands are protected from eviction; recent terminal records may be replaced when that bounded cache fills. Reboot clears local command/feedback state. No browser storage is used for ACK state or receive-window state.

ACK matching includes both sequence and source collar. Superseded, acknowledged and expired queued packets are checked again before transmission. After blind retries, commands wait for another collar RX opportunity until expiry. Cloud retries retain the original deadline and do not revive terminal commands. A cloud command needs a trustworthy hub clock to compare its expiry; without one the hub declines it. These checks do not solve the separately deferred downlink authentication protocol.

## Captive portal and shortcuts

`/welcome#save-shortcut` is linked from dashboard settings. It explains how to open `http://192.168.4.1/` in a normal browser after keeping the no-internet Wi-Fi connection, with `http://bluepaws.local/` as an alternative where supported. It includes Android/Chrome and iPhone/Safari home-screen instructions and a Windows `.url` download. The shortcut requires connection to the hub Wi-Fi. An HTTP captive portal cannot reliably force Chrome/Safari to open or install an OS shortcut; Windows MSN and mobile captive-viewer behavior must still be checked on real devices.

## Verification and rollout

Automated checks cover the actual hub renderer, pure feedback clocks, real cloud card rendering, the actual C++ queue/ACK functions compiled against host stubs, public asset routing, and the real SQL migration in isolated PostgreSQL/WASM with representative RLS fixtures. The loopback UI fixture is synthetic and never sends commands to hardware or cloud.

```powershell
node --test tools/test_hub_device_cards.mjs tools/test_hub_feedback.mjs tools/test_hub_welcome.mjs
py -3.11 -m unittest tools.test_hub_public_assets
node tools/test_hub_command_feedback.mjs
npm install --prefix .pio/feedback-tests --no-package-lock --ignore-scripts @electric-sql/pglite
node tools/test_collar_feedback_db.mjs
npm --prefix web run test:feedback
npm --prefix web run typecheck
npm --prefix web run lint
```

The C++ test uses `CXX` when supplied; otherwise `g++` on Linux or the local MinGW path on Windows. It writes generated build artifacts only under `.pio/feedback-tests`.

Deployment is separate from these tests:

1. Review and apply `20260827143000_add_collar_feedback_snapshot.sql` before the web deployment. No Edge Function redeploy is needed for this change.
2. Merge the feature PR for Vercel deployment.
3. Back up the Home Hub's current LittleFS image, then deploy firmware and public assets **while preserving configuration, credentials and the offline journal**. A plain filesystem upload can erase those files; do not use it without a preservation workflow.
4. Update collar firmware to remove the incorrect normal-report reset-reason TLV. The header and TLV v1.2 contract are unchanged.
5. On hardware, verify a real radio packet, matching ACK, missing ACK, ten-minute expiry and fifteen-minute disappearance. Check cloud replay does not light the bulb; confirm Lost Alert alone is not a fault, while an actual error flag still is.

Local tests and successful builds are not evidence of a production migration, hardware flash or end-to-end radio test.
