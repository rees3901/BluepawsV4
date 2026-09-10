# Hub self-presence and collar receive indicator

## Behaviour

- Cloud and local dashboards include the Home Hub in the same device-card and
  map-marker renderers as collars, with capability-specific fields/actions.
- Defaults: 🏡 Home; 📱 Portable and Off-Grid. Users can edit the name, each
  mode's emoji, and border colour. Hub appearance is separate from collar IDs.
- Cloud edits are Family owner/member-only. Local edits use the hub's existing
  command-access boundary (optional Off-Grid PIN); no credentials reach browsers.
- Cloud settings have a revision. A lightweight authenticated settings read
  checks for changes about every five seconds; after the BLE task applies them,
  an immediate self-report confirms application. The periodic heartbeat remains
  independent. Local overrides persist in NVS
  and are not uploaded as cloud edits. A subsequent explicit cloud edit wins.
- The Bluetooth control enables/disables the **Home beacon preference**.
  Advertising still requires primary Home Wi-Fi; Portable/Off-Grid scanning is
  unchanged. The UI separately shows preference, pending delivery and actual advertising.
- Hub telemetry is attempted at the selected reporting interval while online, independently
  of collar traffic, in the existing cloud task. Network failure/busy HTTP work
  can delay it; it is not a real-time deadline.
- The cloud UI polls hub status every ten seconds, accelerating to two seconds
  while a settings revision awaits confirmation; local UI polls every five.
  A cloud hub without a self-report for its interval plus 30 seconds shows **No contact**, unfilled
  signal bars and a dimmed card. The local hub uses 15 seconds (three missed
  local polls); collars retain their ten-minute threshold. Hub age uses
  the last cloud self-report or successful local API response; failed polls do
  not refresh it. GPS fix age remains separate from contact age.
- Missing cloud reports mean contact is lost, not proof that Wi-Fi specifically
  failed. Power loss, internet loss and a cloud outage can look identical.
  A connected local hub in Off-Grid mode can correctly report **No Wi-Fi**
  uplink while its hotspot and local commands remain available.

## Shared device presentation

The cloud `HubCard` is a settings/data adapter around `DeviceCard`, not a separate
card layout. Hubs participate in the same drag ordering, pin-to-top, four-card
expansion limit, avatar expansion, Jump, Follow and Trail controls. The local
dashboard likewise routes hub data through its existing `updateDevice`,
`renderDeviceCard` and marker/popup functions.

Hub cards show their communications mode, Wi-Fi signal **bars and Wi-Fi badge**,
last-contact stopwatch, coordinates, GPS fix age/time and Home beacon state. The
same battery graphic shows **No data** until actual hub battery reporting is
implemented; it must not display zero volts or an invented percentage. Collar-only
command receive indicator and collar commands are omitted. Hub **Cmd** selects
its independent reporting profile; it never sends a collar command to itself.
Bluetooth preference and editable names retain the hub-specific persistence path.

Message Log and Download use the same three-column report presentation, populated
from the latest `hub_presence` row (cloud) or `/api/hub-presence` response (local).
They are latest-status snapshots, not a claimed archive of previous hub reports.
Cloud report modals use a body-level React portal, escaping the transformed
sidebar and centering on the viewport for both collars and hubs. Indicator groups
use actual compact dimensions, not visual-only scaling; Wi-Fi badges never wrap.
Hub trails currently build from observed live fixes in the browser session, not
collar history. Neither reports nor trails call collar endpoints with hub keys.

The cloud avatar plus button opens the **same AvatarEditorModal** as collars:
searchable emojis, photo upload/cropping and marker colour. Metadata-free 256px
WebP files are saved in the private `hub-avatars` bucket, scoped to Family/gateway.
Photos apply in all modes; choosing an emoji changes the currently edited mode
and returns to mode-specific emoji display. No service key or public photo URL
is used. Family removal denies future downloads. Hub telemetry preserves photos;
Family transfers clear the old private photo reference. Offline name/emoji/colour
remain hub-local; Supabase photo upload is online-only and private cloud photos
are not automatically copied onto the open hotspot.

Negative browser-only hub keys avoid collisions with collar IDs. They never
change the actual gateway ID or enter collar command/history APIs. A hub without
its own GPS fix keeps its card but has no map marker and a disabled Jump control;
adapter placeholder coordinates must never place it at 0,0.

Full card parity requires the `add_hub_avatar_photos` migration before deploying
the web changes, followed by `fix_hub_avatar_policy_lookup`, plus updated hub
public assets. Photo policies use the Family-scoped `hub_presence` table, not
the server-only gateway provisioning registry. No ingestion or collar firmware
change is needed. Preserve existing hub journals/config when updating assets.

## Position integrity

Only the hub's own GNSS may supply its location. No collar position is used as a
substitute or distance origin. Until first fix the card remains visible but has
no map marker. Later no-fix cloud reports preserve the last location and **its
original fix age**, separately from last contact. No GPS coordinates are invented.

Tracker V2 uses UC6580 at 115200 baud, MCU RX33/TX34, reset35, Vext3 HIGH.
These match the known-working legacy receiver's hardware setup and the
[Heltec Tracker V2 datasheet](https://resource.heltec.cn/download/Wireless_Tracker_V2/Wireless_Tracker_v2_Datasheet/Wireless%20Tracker%20v2.pdf).
TinyGPSPlus reads NMEA in a bounded low-priority FreeRTOS task. LoRa remains
independent, higher-priority. Settings alone are persisted on change, not every
GNSS sentence. Real GNSS acquisition still needs a hardware/outdoor check.

## HTTP contract

POST to the existing ingest-position endpoint using the **gateway bearer**, not
a collar token/HMAC. This is a distinct JSON branch, not a TLV format change.

```json
{
  "format": "hub_status",
  "ingest_path": "hub_self",
  "gateway_guid16": "0010",
  "mode": "home",
  "latitude": null,
  "longitude": null,
  "fix_age_s": null,
  "uptime_s": 60,
  "wifi_rssi_dbm": -45,
  "ble_enabled": true,
  "ble_advertising": true,
  "free_heap": 100000,
  "applied_revision": 0,
  "reporting_profile": "power_save",
  "report_interval_s": 180,
  "control_poll_s": 5
}
```

Valid modes: home, portable, off_grid. Coordinates must be a valid pair or both
null. When present, fix_age_s is an integer 0–604800. RSSI can be null.
Gateway identity is four hex digits, nonzero and a multiple of 16.
The handler hashes the bearer, scopes it to that enabled gateway and resolves
Family from the database. Browser roles cannot write telemetry or call ingestion RPCs.
Successful response: HTTP 200, accepted, received_at and settings with revision,
ble_enabled, reporting_profile, display_name, home_emoji, portable_emoji, marker_colour.
Self reports do **not** claim collar commands or enter collar history.

Local endpoints: GET /api/hub-presence; POST /api/hub-preferences
(display_name, home_emoji, portable_emoji, marker_colour, ble_enabled, reporting_profile).
Names/emoji are bounded to 64 UTF-8 bytes in firmware/storage.
Local status also includes `ble_settled`: the requested Home beacon preference
has reached the BLE task's actual mode-dependent advertising state.

### Prompt hub settings (28 August 2026)

The hub makes this small POST to the same endpoint and gateway bearer boundary:

```json
{"format":"hub_settings","ingest_path":"hub_self","gateway_guid16":"0010"}
```

It returns only the existing `settings` object (or null before the first real
hub report). It checks the enabled bearer, enabled gateway, and current Family
before reading that gateway's settings. This is strictly read-only: it does not
refresh presence, write a GPS point, claim collar commands, or acknowledge a
setting. A later real `hub_status` reports the applied revision and BLE state.
No database migration, collar protocol change, or new browser credentials.

REST remains outbound through the router/NAT. A browser cannot assume it can
reach the hub's LAN address. A persistent private WebSocket would need its own
gateway authentication and reconnect design; it is not introduced here.
Five-second polling is a bench/product-development latency choice (up to 720
settings calls/hour per online hub); revisit event delivery/cost before fleet rollout.
Polling shares the existing single cloud worker/TLS connection budget. LoRa
reception stays in its higher-priority task; queued live collar work wins.
Settings reads back off to 60 seconds on failures. Two-second HTTP timeouts and
other cloud work mean five seconds is an aim, not a guaranteed delivery deadline.

The cloud button shows **reported** Bluetooth, not the desired database value.
After saving: updating → hub-confirmed, or an actionable unconfirmed warning
after a bounded confirmation window. At 30 seconds it remains a neutral
“still waiting” message; a warning follows after at least 90 seconds, or the
reported interval plus 30 seconds if longer. A saved but unconfirmed setting remains durable and may apply
after reconnection; the UI explicitly says this, rather than pretending it was
cancelled. A later matching acknowledgement clears that warning. Concurrent
newer settings supersede the older request. No collar one-hour/ten-minute queue
semantics are used for this always-on hub setting.

Local commands go directly to the hub (existing optional PIN boundary) and await
`ble_settled`, with an eight-second confirmation window and request timeouts.
Other local browsers see the updated preference on their next local poll.
Neither setting enables Home advertising in Portable/Off-Grid mode; the existing
primary-Home-Wi-Fi safety gate remains unchanged.

## Hub reporting power profiles (28 August 2026)

| Profile | Self-report interval | Cloud contact overdue |
| --- | --- | --- |
| Power Save (new firmware/default preference) | 180 seconds | 210 seconds |
| Normal | 60 seconds | 90 seconds |
| Active | 30 seconds | 60 seconds |

These are **reporting profiles**, independent of Home/Portable/Off-Grid. There
is no Lost Alert or Debug hub profile. They do not sleep the hub or slow LoRa RX,
GNSS reading, BLE, the captive portal, or five-second settings checks. Consequently
Power Save reduces self-report traffic, not all hub power consumption.
Local status remains available every five seconds; its contact timeout remains
15 seconds regardless of cloud reporting cadence.

Firmware persists the profile only on settings changes in NVS. A profile/BLE
change requests a prompt confirmation report, bypassing the long periodic
interval. Cloud settings store desired and reported profiles separately; the
browser needs both a matching value and applied revision before confirming.
Existing Family owner/member RLS restricts preference edits; browsers cannot
write reported profiles, capability, telemetry or applied revisions.

Old firmware without profile fields is treated as Normal, not falsely labelled
Power Save. It reports no control capability, so the cloud profile dialog asks
for a firmware update. The RPC keeps defaults for older Edge callers; the new
Edge handler rejects unknown/Lost/Debug hub profiles. No TLV bytes or collar
power-profile codes change.

## Collar 💡 / 💤 and last-seen

Both dashboards show 💡 beside signal quality during the remaining
ten-second receive opportunity, then 💤 for “probably sleeping”, not confirmed
sleep. Tooltip/accessible label explain the estimate. Hubs never show this indicator.
Cloud timing uses original live reception, not the collar's GNSS clock.
Historical replay, repeated snapshots and duplicate uploads do not renew it.
Missing trustworthy LoRa reception time remains conservative. Internet latency
can shorten or consume the indicator; it is not a promise of command delivery.
Local timing remains monotonic and radio-driven.

The last-seen counter starts immediately at receipt and continues through the
bulb/sleep transition; it does not wait ten seconds or a minute. The 28 August
investigation found collar timestamps almost five minutes ahead of cloud receipt,
which caused the zero-clamped counter to look frozen. The position-only fallback
now uses the earlier of recorded/received timestamps; authoritative device
presence overlays newer contact. Historical replay retains its original age;
upload time alone cannot make an old point fresh. GNSS coordinates are unchanged.

## Rollout and verification

For **reporting profiles, contact clock and sleep indicator** (this update):

1. Review `npx --yes supabase@latest db push --linked --dry-run`, then apply
   `npx --yes supabase@latest db push --linked`. The new migration is
   `20260828102745_hub_reporting_profiles.sql`; review any other pending migrations.
2. Deploy `ingest-position` from this branch/merged main using the command below.
   Migration must precede this new Edge handler.
3. Merge for Vercel and update Home Hub firmware plus public assets, **preserving
   its existing journal/config**. No collar flash is needed.
4. Confirm `/api/hub-presence` includes `reporting_profile`, `control_poll_s: 5`
   and `ble_settled`. Test all three profiles, reboot persistence, BLE confirmation
   and uninterrupted collar reception on real hardware.

The connected hub inspected during development lacked `ble_settled`, indicating
older firmware without the prompt-control path. Changing the website alone
cannot speed up that image. WebSockets remain a possible later improvement:
they reduce polling traffic/latency, but need gateway-scoped authorization,
reconnect/token recovery and durable revision reconciliation. REST is retained
here; its five-second polling cost should be revisited before a fleet rollout.

Regression additions: `tools/test_hub_reporting.cpp` tests the real firmware
cadence helper and millis rollover. `tools/test_collar_feedback_db.mjs` applies
the real new migration in isolated PostgreSQL/WASM and checks legacy calls,
profile validation, Family isolation and write privileges. Browser fixtures use
synthetic data only: `tools/hub_controls_preview.mjs` (cloud React controls) and
`tools/hub_feedback_preview.py` (offline web GUI). Neither connects to hardware
or Supabase.

For the **earlier prompt Bluetooth/card layout update only**:

1. Deploy `ingest-position` from this branch or the merged updated main:
   `npx --yes supabase@latest functions deploy ingest-position --project-ref ykcdaonkvwemedotdpdr`.
2. Merge the PR for the Vercel frontend update.
3. Build/upload the Home Hub firmware using the existing hub procedure; update
   public assets with a fresh private-files/journal-preserving filesystem image.
   **Do not use a plain uploadfs on a configured hub.**
4. No new SQL migration or collar firmware flash is required for this update.

Verify with Home Wi-Fi: toggle Bluetooth, observe settings revision in serial,
then a prompt `[HUB SELF]` success and confirmed UI state. Check actual BLE beacon
advertising stops/resumes. Disconnect hub uplink: after 90 seconds the cloud card
must not retain green Wi-Fi bars; an unconfirmed request must never claim success.
Use the hotspot to repeat the local toggle without cloud access. Check 15-second
local contact-loss indication after disconnecting that browser from the hub.
Hardware timing/real BLE acceptance still needs this check after flashing.

Automated controls regression: `npm --prefix web run test:hub-controls`.

For the **card/photo parity update** on a hub already reporting successfully:

1. From the updated feature branch, run the migration dry-run and push below.
2. Merge the PR so Vercel deploys the matching web code.
3. Update the hub's public assets using the preservation procedure. No firmware
   or Edge Function redeployment is required for this card-only update.

```powershell
npx --yes supabase@latest db push --linked --dry-run
npx --yes supabase@latest db push --linked
```

For the **initial self-presence installation**, after merging and pulling main,
from the repository root:

```powershell
npx --yes supabase@latest db push --linked --dry-run
npx --yes supabase@latest db push --linked
npx --yes supabase@latest functions deploy ingest-position --project-ref ykcdaonkvwemedotdpdr
```

Vercel deploys the merged web code. Build/upload Home Hub firmware and public
assets using the normal hub procedure; **no collar firmware update is needed**.
Do not blindly upload a fresh filesystem image over existing hub journals/config:
use the repository's preservation procedure or back up that state first.

Automated:

```powershell
npm --prefix web run typecheck
npm --prefix web run lint
npm --prefix web test
npm --prefix web run test:feedback
node tools/test_collar_feedback_db.mjs
py -3.11 -m unittest tools.test_hub_public_assets
pio run -e hub
pio run -e hub -t buildfs
```

Database tests use isolated PGlite (see test header for installation), not live
Supabase. tools/hub_feedback_preview.py is a loopback-only synthetic UI fixture.
Hardware acceptance: confirm profile-paced [HUB SELF] 200 responses, genuine GNSS
fix/age, mode icons, reboot-persistent appearance, BLE gate, command priority, and
💡 → 💤 without historical replay relighting it.
