# Bluepaws V4 Collar Downlink Command Contract

This document locks the first production-shaped command-routing model for collar control. It covers LTE-direct responses, Home Hub relay, command identity, ACK flow, and the first safe command dictionary.

## Core routing rule

Collars are not directly addressable servers. They sleep, they do not have stable public IP addresses, and they may sit behind carrier NAT. Commands are therefore routed by `device_id`, not by IP address.

New dashboard power-profile commands expire after ten minutes. The generic database queue retains its existing one-hour default for other callers; the dashboard explicitly requests ten minutes. A command can only be delivered when the collar creates a delivery opportunity by waking and checking in. Queueing is therefore immediate, while radio delivery is opportunistic and retryable.

```text
User or system queues command for device 1001
  ↓
Supabase stores command as pending
  ↓
Collar wakes and sends TLV check-in / telemetry
  ↓
Ingestion authenticates the collar packet
  ↓
Backend returns or exposes the next pending command
  ↓
Collar applies the command and sends an ACK
  ↓
Backend marks the command acknowledged
```

## LTE-direct delivery

For `ingest_path = cellular_direct`, the collar sends the normal HTTPS JSON wrapper containing the base64 TLV payload.

If a pending command exists, the Edge Function includes it in the JSON response:

```json
{
  "accepted": true,
  "format": "tlv",
  "device_id": 1001,
  "message_id": 572,
  "command_pending": true,
  "command": {
    "id": "3e0f2d6d-7f93-4b8f-a527-0b63ddad71d5",
    "sequence_id": 1234,
    "type": "set_profile",
    "payload": {
      "profile": "active"
    },
    "expires_at": "2026-08-24T18:30:00Z"
  }
}
```

If no command exists:

```json
{
  "accepted": true,
  "command_pending": false,
  "command": null
}
```

The collar then ACKs with a TLV v1.2 packet using:

- `tx_reason = ACK`
- `TLV_ACKED_MSG_SEQ_ID = command.sequence_id`

The backend keeps the UUID command ID for audit and web UI state, but the embedded ACK uses the compact 16-bit sequence value.

## Home Hub delivery

For `ingest_path = lora_hub`, the collar sends raw TLV over private LoRa. The Home Hub receives it, wraps it for HTTPS, and relays it to Supabase.

The Home Hub's authenticated telemetry POST is also the command check. The Edge Function returns the next pending command in that HTTP response, and the Home Hub immediately transmits the corresponding TLV v1.2 downlink during the collar's 10-second RX window. If delivery or the ACK is missed, the same command remains retryable until acknowledged, superseded, or expired.

The collar ACK format remains the same:

- `tx_reason = ACK`
- `TLV_ACKED_MSG_SEQ_ID = command.sequence_id`

This fits TLV v1.2 without consuming another header flag:

- profile commands use `source_id16 = hub`, `destination_id16 = collar`, `tx_reason = CONFIG`, and `TLV_PROFILE`;
- acknowledgements reverse the addresses and use `tx_reason = ACK` plus `TLV_ACKED_MSG_SEQ_ID`;
- the command header `message_sequence_id` is the backend's compact 16-bit command sequence;
- a repeated sequence is not applied twice, but is ACKed again in case the earlier ACK was lost.

## Initial command dictionary

These are the first supported backend command types. They are intentionally conservative.

| Command type | Payload | Meaning | First delivery path |
|---|---|---|---|
| `set_profile` | `{ "profile": "normal" \| "power_save" \| "active" \| "lost_alert" \| "debug" }` | Change collar power/safety profile. `debug` is development-only and must not be customer-facing. | LTE response and Home Hub |
| `request_status` | `{}` | Ask collar to reply with battery/profile/status diagnostics. | Home Hub first |
| `force_report` | `{ "gnss": true \| false }` | Ask collar to send a fresh report at the next safe opportunity. | Later |
| `enter_lost_alert` | `{}` | Enter Lost Alert emergency search mode. | LTE response and Home Hub |
| `exit_lost_alert` | `{ "fallback_profile": "normal" \| "power_save" \| "active" }` | Leave Lost Alert and return to a non-emergency profile. | LTE response and Home Hub |
| `reboot` | `{ "reason": "owner_request" \| "support" }` | Reboot collar firmware. Owner/support only. | Later |
| `debug_cadence` | `{ "enabled": true \| false, "interval_s": 15 }` | Prototype/testbed faster wake cadence. Must not be production-user-facing. | Testbed only |

## Explicitly deferred commands

The following are useful but should not be exposed as general customer controls yet:

- arbitrary RF transmit power changes;
- arbitrary LoRa spreading factor/bandwidth/coding-rate changes;
- raw modem AT command passthrough;
- direct APN edits from the customer web app;
- factory reset.

These can strand a collar, break regulatory assumptions, or create support/security risk. They should be support-only or manufacturing-only workflows later.

## Command state machine

```text
pending
  ↓ claimed by delivery path
sent
  ↓ collar ACK received
acked

pending/sent
  ↓ expires_at passes
expired

pending/sent
  ↓ owner/support revokes
cancelled

sent
  ↓ too many delivery attempts
failed
```

New customer power-profile commands expire ten minutes after creation. The local Home Hub also expires its commands after ten minutes and respects an earlier cloud expiry. Queueing a newer profile command for the same collar cancels any older unacknowledged profile command so a stale selection cannot be applied later. Per-card feedback disappears fifteen minutes after submission, not fifteen minutes after expiry. See [Collar UI feedback](COLLAR_UI_FEEDBACK.md) for source and freshness rules.

## Security notes

- The command queue is keyed by `device_id` and `household_id`.
- A command can only be queued for a device in the caller's Family.
- Test/debug commands must remain restricted; `debug_cadence`, `reboot`, and `set_profile` to `debug` are owner-only in the first queue implementation.
- LTE response delivery is protected by the collar's existing bearer token and packet HMAC.
- LoRa downlink command authentication is still a separate protocol decision and must not be treated as production-secure until formally solved.

## Cloud queue permission regression (2026-08-27)

`permission denied for function bluepaws_queue_device_command` can occur before
anything reaches the hub or collar. The browser calls a public SECURITY INVOKER
wrapper, which calls the guarded private SECURITY DEFINER implementation. The
authenticated role needs EXECUTE on both functions. Local/off-grid hub commands
do not use this database path, so their success does not verify cloud permissions.

Migration `20260827213901_fix_cloud_command_queue_permissions.sql` restores only
those queue permissions. It also explicitly rejects NULL/missing Family membership
before enabling the private entry point, and rejects NULL expiry. Anonymous access,
direct table writes and delivery/ACK helper access remain restricted. The public
wrapper stays SECURITY INVOKER; do not solve this error by giving browser clients a
service-role key, disabling RLS or granting broad table permissions.

Run the isolated regression test from the repository root:

```powershell
npm install --prefix .pio/feedback-tests --no-package-lock --ignore-scripts @electric-sql/pglite
node tools/test_command_queue_permissions_db.mjs
```

The test reproduces the pre-fix permission failure, then checks owner/member success,
missing/revoked/guest/cross-Family denial, privileged-command restrictions, expiry,
supersession and least-privilege grants using real PostgreSQL roles in PGlite.

Deployment: apply the migration with the normal reviewed database migration process.
No Edge Function, Vercel or firmware deployment is required for this permission fix.
It was applied to the linked project on 2026-08-27 and verified through the public RPC
as an authenticated owner; a missing-member call was rejected. All live test writes
were rolled back, so no test command was delivered. Hardware delivery/ACK still needs
the normal end-to-end test after a user submits a command.

The post-deployment security advisor also flags unrelated items for separate review:
the [search-party snapshot's public definer access](https://supabase.com/docs/guides/database/database-linter?lint=0028_anon_security_definer_function_executable),
[leaked-password protection](https://supabase.com/docs/guides/auth/password-security#password-strength-and-leaked-password-protection),
and [RLS tables without client policies](https://supabase.com/docs/guides/database/database-linter?lint=0008_rls_enabled_no_policy).
This change does not alter those features or open credential-table access.
