# Collar fault summary

Both dashboards show a short `Reported fault — …` line beneath the name/status
row. It is visible in collapsed and expanded cards. Multiple indicators show the
first reason plus `+1`; the tooltip and accessible label contain all details.

The badge still requires the packet's `ERROR_PRESENT` bit. Lost status, Lost Alert
profile, poor RSSI/SNR, old map coordinates and reset diagnostics alone do not
create an active fault.

| Packet evidence | Summary when ERROR_PRESENT is set |
|---|---|
| STALE_FIX | stale GPS |
| GNSS_VALID clear on TELEMETRY, INTERRUPT, BOOT or ALERT | GPS fix unavailable |
| LOW_BATTERY | low battery |
| No supported indicator / incomplete diagnostics | cause unspecified |

Stale GPS takes precedence over unavailable GPS. ACK, PING, CONFIG and
WAKE_CHECKIN may omit GNSS intentionally, so a clear GNSS_VALID bit on those
reports does not imply GPS failure. Accompanying indicators are not a diagnosis
of the root cause. No age, voltage or radio-quality thresholds are invented.

The optional `reset_reason` TLV appears only as a hexadecimal reset diagnostic
in the tooltip. The current RAK collar sends the low byte of nRF RESETREAS,
not a portable error enum. It does not establish brownout, boot loop, RF failure
or cellular failure. Those need an agreed protocol extension or additional
evidence; the collar's internal `lastError` category is not transmitted today.

## Data and compatibility

- Cloud: the existing Family-scoped feedback RPC supplies the observation ID and
  flags. Only faulty observations need an additional batched, Family-filtered,
  RLS-protected read of report type and the reset diagnostic. IDs, device IDs and
  flags must match the snapshot. Failed/missing reads retain the fault flag with
  only the detail supported by that flag. No raw packets or secrets are selected.
- A newer no-GPS presence update discards the old map position's diagnostic
  context. Never combine its coordinates/flags with a newer fault report.
- Local: live SSE, reconnect SSE and `/api/devices` expose `flags`,
  `txReasonCode` and `resetReasonPresent`. The existing live SSE `txReason`
  display string and `errorPresent` field remain compatible. Older hub firmware
  falls back to `cause unspecified`, without guessing from retained GPS data.

## Rollout and checks

No SQL migration, Edge Function change, credential change or collar flash is
required. Deploy the cloud frontend normally. For local detail, update the Home
Hub firmware **and** its public assets using the existing private-file/journal
preservation procedure. Do not run a plain `uploadfs` on a configured hub.

Checks: `npm --prefix web test`, `npm --prefix web run test:feedback`,
`node --test tools/test_hub_device_cards.mjs`, web lint/typecheck and `pio run -e hub`.
The local/cloud summary rules are compared across every flags byte and TX reason.

Synthetic browser fixtures (no hardware or cloud):

- `node tools/shared_device_card_fixture.mjs --serve`, then `/faults` on port 8793.
- `python tools/hub_feedback_preview.py --faults`, port 8792.

Live acceptance after deployment: send a stale-fix fault, a healthy report, a
faulty wake check-in and a low-battery fault; verify the badge updates/clears and
survives reconnect with the same meaning. These fixtures do not certify the
deployed radio/cloud pipeline.
