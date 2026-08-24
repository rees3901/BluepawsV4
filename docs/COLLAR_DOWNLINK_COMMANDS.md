# Bluepaws V4 Collar Downlink Command Contract

This document locks the first production-shaped command-routing model for collar control. It covers LTE-direct responses, Home Hub relay, command identity, ACK flow, and the first safe command dictionary.

## Core routing rule

Collars are not directly addressable servers. They sleep, they do not have stable public IP addresses, and they may sit behind carrier NAT. Commands are therefore routed by `device_id`, not by IP address.

The backend stores pending commands for a collar. A command can only be delivered when the collar creates a delivery opportunity by waking and checking in.

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

The collar then ACKs with a normal TLV v1.1 packet using:

- `tx_reason = ACK`
- `TLV_ACKED_MSG_SEQ_ID = command.sequence_id`

The backend keeps the UUID command ID for audit and web UI state, but the embedded ACK uses the compact 16-bit sequence value.

## Home Hub delivery

For `ingest_path = lora_hub`, the collar sends raw TLV over private LoRa. The Home Hub receives it, wraps it for HTTPS, and relays it to Supabase.

The first implementation should let the Home Hub poll for pending commands by `device_id`, then transmit the command during the collar's RX window. Later, this can be upgraded to Realtime/WebSocket delivery to the hub.

The collar ACK format remains the same:

- `tx_reason = ACK`
- `TLV_ACKED_MSG_SEQ_ID = command.sequence_id`

## Initial command dictionary

These are the first supported backend command types. They are intentionally conservative.

| Command type | Payload | Meaning | First delivery path |
|---|---|---|---|
| `set_profile` | `{ "profile": "normal" \| "power_save" \| "active" \| "lost_alert" }` | Change collar power/safety profile. | LTE response and Home Hub |
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

## Security notes

- The command queue is keyed by `device_id` and `household_id`.
- A command can only be queued for a device in the caller's Family.
- Test/debug commands must remain restricted.
- LTE response delivery is protected by the collar's existing bearer token and packet HMAC.
- LoRa downlink command authentication is still a separate protocol decision and must not be treated as production-secure until formally solved.

