# BluePaws Ping and LoRa Wake-on-Radio Implementation

Status: implementation specification

Scope: cloud web GUI, Home Hub local/offline GUI, Home Hub front display, Supabase, Home Hub firmware, and collar LoRa/nRF52840 firmware
Out of scope: cellular delivery and cellular-initiated wake-up

## 1. Objective

An authorised user can select a specific collar and press **Ping**. The always-on Home Hub obtains that command and transmits an addressed LoRa downlink with a 64-symbol preamble. The collar SX1262, operating in receive-duty-cycle mode, detects the long preamble and raises DIO1 after receiving the packet. DIO1 wakes the nRF52840, which acknowledges the command, attempts a fresh GNSS fix, and returns an authenticated report. The user interfaces show the progress and result.

The feature is deliberately Home-Hub-dependent. If no assigned Home Hub is online and within LoRa range, an immediate Ping cannot be delivered. Cellular is not a parallel wake path in this milestone.

## 2. Existing BluePaws concepts to retain

The repository already contains most of the required foundations:

- `public.device_commands` is the durable command queue, addressed by `household_id` and `device_id`.
- `bluepaws_queue_device_command(...)` is the authorised RPC used by the cloud GUI.
- The command lifecycle is `pending -> sent -> acked`, with expiry/failure/cancellation states.
- TLV v1.2 has distinct `source_id16` and `destination_id16` fields.
- `TX_PING` is value 2 and is currently aliased as `PKT_CMD_STATUS`.
- The database currently calls the related cloud command `request_status`.
- The Home Hub already has a LoRa command queue, retry/ACK tracking, and `sendStatusCommand()`.
- The collar already handles `PKT_CMD_STATUS` and returns `PKT_STATUS_RESP`.
- `TX_WAKE_CHECKIN` means a scheduled, collar-originated lightweight check-in. It is not the response to an on-demand Ping and should not be reused for this feature.

There is currently a semantic gap: `request_status`/`PKT_CMD_STATUS` returns configuration status without acquiring a fresh GNSS fix, whereas the desired Ping operation means “wake this collar and return a fresh position/status report.” This milestone must close that gap explicitly.

## 3. Canonical Ping semantics

Use **Ping** as the product/UI name and `ping` as the canonical cloud command type.

For minimum radio-protocol churn in the first implementation:

- Map cloud command type `ping` to the existing LoRa `PKT_CMD_STATUS` / `TX_PING` value 2.
- Treat legacy cloud `request_status` as an alias during migration, but do not expose both commands in the UI.
- Update the collar handling of `PKT_CMD_STATUS` so it performs the new Ping workflow rather than returning configuration-only status.
- Do not use `TX_INTERRUPT` for the response. `TX_INTERRUPT` currently represents local/user hardware interruption and `PKT_CMD_FIND` compatibility.
- Do not use `TX_WAKE_CHECKIN`; it remains reserved for scheduled check-ins.
- Use `TX_PING` for the Ping-triggered report, with `TLV_ACKED_MSG_SEQ_ID` containing the command sequence that caused it.

Longer term, packet type and transmit reason should be separated rather than relying on their current compatibility aliases. That cleanup is not required to prove wake-on-radio, but new code must not add further ambiguous aliases.

## 4. End-to-end flow

```text
Cloud web marker card                       Local/offline GUI or hub display
          |                                             |
          | queue authenticated `ping`                  | queue local `ping`
          v                                             v
 Supabase device_commands                    Home Hub command queue
          |                                             |
          | assigned Home Hub polls                     |
          +---------------------> Home Hub <-------------+
                                      |
                                      | addressed LoRa command
                                      | 64-symbol preamble
                                      v
                         Collar SX1262 RxDutyCycle
                                      |
                              valid packet / DIO1
                                      v
                           nRF52840 System ON wake
                                      |
                         immediate receipt ACK/status
                                      |
                            bounded fresh GNSS attempt
                                      |
                         TX_PING telemetry over LoRa
                                      v
                            Home Hub -> Supabase
                                      |
                          GUI/display shows result
```

Only one logical command identity may exist for a cloud Ping. Retries use the same UUID and 16-bit command sequence. The collar must never acquire repeated fixes or perform repeated side effects for duplicate deliveries, but it should repeat the appropriate ACK/response when necessary.

## 5. Cloud web GUI requirements

Add a **Ping** action to the specific collar’s marker card/device card. Do not offer it on the Home Hub’s own card.

On activation:

1. Require an authenticated Family member with permission to control that collar.
2. Call `bluepaws_queue_device_command` with:

   ```json
   {
     "requested_device_id": 1001,
     "requested_command_type": "ping",
     "requested_payload": {},
     "requested_ttl": "2 minutes"
   }
   ```

3. Disable repeated clicks while the same collar has an unexpired Ping in `pending` or `sent` state.
4. Show honest state, without optimistically moving the marker:

   - `Queueing ping…`
   - `Waiting for Home Hub`
   - `Sent over LoRa`
   - `Collar reached; acquiring location`
   - `Ping complete` with response time and fix freshness
   - `No fresh GPS fix; collar reached` when the bounded fix attempt fails
   - `Collar not reached` on expiry
   - `Home Hub offline` when the assigned hub is known to be unavailable

5. Subscribe to the existing private Family command/telemetry feedback where available, with ordinary refresh as a recovery path.

Two minutes is recommended for Ping rather than the existing ten-minute profile-command lifetime. Ping is an immediate, user-observed action; a much later surprise execution would be misleading.

## 6. Local/offline GUI and Home Hub front-display requirements

The same **Ping** action must appear in:

- the Home Hub’s local/offline device marker or card;
- the Home Hub front-display device controls.

These interfaces already run on or communicate directly with the target Home Hub, so they should enqueue the same local radio command without requiring Supabase availability. Add or standardise a local endpoint such as:

```http
POST /api/ping
Content-Type: application/x-www-form-urlencoded

device=03E9
```

The endpoint must apply the existing local command-access controls, validate that the ID belongs to a collar associated with the hub, allocate a sequence, add the command to the existing bounded command/ACK tracker, and return HTTP `202` only when accepted into that queue.

When online, the local UI may also mirror command/result state to Supabase for audit, but cloud failure must not prevent a local Ping. Avoid creating a second radio command when mirroring.

The local GUI and physical display should use the same visible states as the cloud GUI wherever possible.

## 7. Supabase requirements

### 7.1 Command schema and validation

Add `ping` to the allowed `device_commands.command_type` values and validate that its payload is exactly `{}`. Continue accepting `request_status` temporarily if existing clients/tests depend on it.

The queue RPC must:

- enforce Family membership and command permissions;
- resolve the collar’s assigned Home Hub;
- reject hub IDs and broadcast IDs as Ping targets;
- allocate the existing compact command sequence;
- cancel or coalesce an older unacknowledged Ping for the same collar;
- use a short Ping expiry, recommended two minutes;
- publish command-state feedback to authorised Family subscribers.

### 7.2 Hub command retrieval

Provide an authenticated, hub-scoped command retrieval operation. The Home Hub identifies itself with its provisioned gateway credential and asks only for commands belonging to collars assigned to it. The response must never expose another household’s commands.

A suitable request/response shape is:

```json
{
  "format": "hub_commands",
  "hub_id": 61441,
  "limit": 4
}
```

```json
{
  "format": "hub_commands",
  "commands": [
    {
      "id": "uuid",
      "device_id": 1001,
      "sequence_id": 1234,
      "type": "ping",
      "payload": {},
      "expires_at": "..."
    }
  ]
}
```

Claiming must be atomic enough to prevent multiple hubs from independently originating the same first transmission. Redelivery of an unacknowledged command is allowed and retains the original identity and expiry.

### 7.3 Completion

Mark the command `sent` only after the Home Hub confirms successful SX1262 transmission. Mark it `acked` when Supabase receives an authenticated collar packet containing the matching source collar ID and `TLV_ACKED_MSG_SEQ_ID`.

Store the Ping result separately from delivery state where necessary:

- command delivery proves the collar received it;
- a subsequent Ping report proves the collar responded;
- `GNSS_VALID` determines whether that response contains a fresh usable position.

## 8. Home Hub cloud polling

The present hub cloud task wakes every five seconds for housekeeping, but it does not independently retrieve arbitrary pending commands; cloud commands are mainly discovered in responses to collar telemetry. Ping requires a new, proactive hub command retrieval path.

Do not use a fixed 30-second interval as the primary Ping design. It adds up to 30 seconds of avoidable user-visible delay. The hub is mains-powered, so its local power cost is negligible; the concerns are cloud request volume and fleet scaling.

Recommended first implementation:

- poll the hub-scoped command endpoint every **5 seconds** while Wi-Fi/cloud connectivity is healthy;
- add per-hub random jitter so a fleet does not poll simultaneously;
- immediately poll again after reconnecting or after a GUI notification indicates new work;
- use exponential backoff from 5 seconds to 5 minutes on network/server failures;
- return compact empty responses (`204 No Content` where practical);
- make the base interval remotely configurable;
- later replace or supplement polling with a private Realtime notification or long-lived event channel, retaining slower polling as recovery.

At a five-second interval the expected cloud-to-radio dispatch latency is normally below five seconds. A 30-second fallback interval is reasonable only after a reliable push notification mechanism exists.

## 9. Home Hub LoRa transmission

When the hub receives a valid `ping` command:

1. Confirm that it is not expired, cancelled, acknowledged, or already active locally.
2. Convert it to the existing addressed `PKT_CMD_STATUS`/`TX_PING` TLV v1.2 packet.
3. Set:

   - `source_id16` to the assigned Home Hub ID;
   - `destination_id16` to the requested collar ID;
   - `message_sequence_id` to the Supabase command sequence;
   - no broadcast destination;
   - a valid production command authentication tag.

4. Under the existing LoRa/SPI mutex:

   - change the SX1262 transmit preamble to **64 symbols**;
   - transmit the Ping packet;
   - restore the hub’s ordinary eight-symbol preamble immediately afterwards;
   - restore continuous hub receive mode.

5. Register the command with the existing ACK tracker and apply bounded retries using the same command sequence.
6. Report `sent`, retry, expiry, and acknowledgement state to Supabase and the local UI.

The 64-symbol preamble is only for hub-to-collar wake commands. Ordinary collar telemetry and normal hub traffic remain at eight symbols unless a separate design decision changes them.

At SF10 and 125 kHz bandwidth, one symbol lasts approximately 8.192 ms and a 64-symbol preamble lasts approximately 524 ms before the remaining LoRa preamble/header/payload airtime. Every retry therefore has a material airtime cost. Retries must be bounded and the final channel-access/duty-cycle behaviour must be validated for the deployment region.

## 10. Collar SX1262 and nRF52840 behaviour

### 10.1 Sleep/listen state

Replace ordinary `lora.sleep()` during the ping-reachable collar state with SX1262 receive-duty-cycle operation:

```cpp
lora.setPreambleLength(64);
lora.setPacketReceivedAction(onLoRaWakePacket);
lora.startReceiveDutyCycleAuto(64, 8);
```

Exact RadioLib calls and periods must be verified against the pinned RadioLib version and real RAK4630. The receiver must also be configured to expect the 64-symbol preamble; configuring only the hub for 64 symbols is not the production-reliable arrangement.

Keep the nRF52840 in low-power **System ON** sleep with the RAK4630’s internally connected SX1262 DIO1/P1.15 configured as a wake-capable GPIO interrupt. Do not start with System OFF: its reset-style wake complicates preservation and reading of the SX1262 receive buffer.

Configure DIO1 for `RX_DONE` as the normal MCU wake event. Let the SX1262 perform preamble detection and full packet reception autonomously. Waking the MCU on `PreambleDetected` would cause more false wakes and is unnecessary.

Record an internal diagnostic wake cause such as `LORA_DIO1_RX` or `RF_WAKE`. This is a firmware/reset diagnostic, not a new TLV transmit reason.

### 10.2 Ping handling

After DIO1 wakes the MCU:

1. Read the SX1262 packet before reinitialising or sleeping the radio.
2. Validate length, TLV v1.2 structure, destination ID, source hub assignment, command sequence, expiry/freshness information where present, and authentication.
3. Ignore unauthenticated, misaddressed, malformed, expired, or replayed commands.
4. For a duplicate valid Ping, do not repeat an expensive GNSS acquisition if the previous result is still available; re-ACK/re-send the cached result where practical.
5. Send a prompt receipt ACK so hub retries do not continue throughout GNSS acquisition.
6. Attempt a fresh GNSS fix using a bounded timeout and the established GM02SP GNSS policy. Cellular data delivery remains out of scope; only GNSS positioning is used here.
7. Send an authenticated LoRa report with:

   - `tx_reason = TX_PING`;
   - `destination_id16 = originating hub ID`;
   - `TLV_ACKED_MSG_SEQ_ID = received Ping sequence`;
   - current battery, status/profile, activity and diagnostic fields that fit the locked packet budget;
   - fresh coordinates and `GNSS_VALID` when a fix succeeds;
   - last known coordinates marked stale, or a no-position response, when the bounded fix fails.

8. Restore the 64-symbol receive-duty-cycle configuration and return the nRF52840 to System ON sleep.

If the project chooses to avoid two response packets, the single final Ping response may also serve as the ACK, but the hub ACK timeout must then exceed the worst permitted GNSS acquisition time. A prompt receipt ACK followed by a result report is preferred because it separates radio delivery from GNSS success.

## 11. Security requirements

The current collar source explicitly states that production downlink authentication is deferred. Ping creates an always-listening command surface, so production release must not rely only on CRC, source ID, or a private LoRa sync word.

Before customer deployment:

- authenticate every command with the collar-specific/shared provisioned key;
- include source, destination, command type, sequence and payload in the authenticated bytes;
- persist sufficient replay/deduplication state across nRF52840 resets;
- authorise the hub-to-collar relationship;
- rate-limit cloud and local Ping requests;
- never permit broadcast Ping;
- audit requester, target, command identity, timestamps and outcome.

## 12. Failure and user-visible behaviour

| Condition | Required result |
|---|---|
| Hub offline | Cloud command remains pending briefly; GUI says Home Hub offline/waiting. |
| Hub online, collar out of range | Bounded retries, then `Collar not reached`; do not claim a Ping response. |
| Collar ACKs, GNSS fix succeeds | `Ping complete`; update marker with the fresh fix. |
| Collar ACKs, GNSS fix fails | `Collar reached; no fresh GPS fix`; preserve the previous marker and show its age. |
| Duplicate command/retry | Re-ACK or replay cached result; do not repeat the fix unnecessarily. |
| Cloud unavailable but local GUI works | Local Ping proceeds and is shown locally; reconcile audit state later. |
| Hub transmission fails locally | Keep retryable until bounded retry/expiry; do not mark cloud state `sent`. |

## 13. Implementation sequence

1. Lock the Ping semantics in the TLV/downlink documentation and add protocol tests.
2. Add the Supabase `ping` type, validation, short TTL, coalescing, hub-scoped claim endpoint, RLS/security tests, and feedback events.
3. Add the cloud marker-card Ping action and state rendering.
4. Add the Home Hub’s five-second command poller with jitter/backoff.
5. Map cloud `ping` to the existing addressed status/Ping radio packet and add 64-symbol transmit-preamble switching with guaranteed restoration.
6. Add `/api/ping` to the local/offline GUI and the corresponding front-display action.
7. Implement collar SX1262 receive-duty-cycle operation and nRF52840 DIO1 System ON wake.
8. Change collar Ping handling to prompt ACK plus bounded GNSS acquisition and `TX_PING` result telemetry.
9. Add end-to-end feedback and command completion processing.
10. Bench-test current, latency, packet-loss, duplicates, timeouts and recovery before enabling the feature by default.

## 14. Acceptance criteria

- Ping is available on each collar card in cloud and local/offline UIs and on the Home Hub display.
- A cloud Ping produces exactly one durable logical command addressed to the selected collar.
- A healthy Home Hub normally begins LoRa transmission within five seconds of cloud queueing.
- The hub transmits Ping with a 64-symbol preamble and restores its normal preamble afterwards.
- A sleeping RAK4630 collar is woken through SX1262 DIO1 without a periodic nRF52840 application wake.
- A command for another device does not wake the application into executing it.
- The collar promptly proves receipt, then returns a fresh fix or an explicit no-fresh-fix result.
- The response is correlated using collar ID and command sequence.
- Duplicate delivery does not trigger duplicate GNSS work or state changes.
- The UI distinguishes queued, transmitted, reached, fresh-fix success, no-fix, timeout and offline-hub outcomes.
- All command and response packets are authenticated before production use.
- Measured collar current and wake reliability are recorded for at least 64-symbol/8-symbol detection operation at the production SF/BW settings.

## 15. Bench validation

Measure rather than infer the final power and reliability:

- idle current with ordinary SX1262 sleep versus receive-duty-cycle mode;
- nRF52840 System ON current with DIO1 armed;
- detection success over at least 1,000 Pings at representative signal levels;
- worst-case cloud poll-to-LoRa latency;
- LoRa command-to-DIO1 latency;
- false wakes in the intended RF environment;
- GNSS time-to-fix and no-fix timeout;
- retry and duplicate behaviour with deliberately dropped ACKs;
- hub preamble restoration after success, timeout and radio error;
- regulatory airtime/channel-access compliance for the selected frequency and retry policy.

The initial 64-symbol choice is a prototype starting point. Keep it configurable until current consumption, latency and missed-wake measurements justify locking it.
