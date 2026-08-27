# Hub self-presence and collar receive indicator

## Behaviour

- Cloud and local dashboards include the Home Hub in the same device-card and
  map-marker renderers as collars, with capability-specific fields/actions.
- Defaults: 🏡 Home; 📱 Portable and Off-Grid. Users can edit the name, each
  mode's emoji, and border colour. Hub appearance is separate from collar IDs.
- Cloud edits are Family owner/member-only. Local edits use the hub's existing
  command-access boundary (optional Off-Grid PIN); no credentials reach browsers.
- Cloud settings have a revision. A self-report response delivers newer settings;
  the following self-report confirms application. Local overrides persist in NVS
  and are not uploaded as cloud edits. A subsequent explicit cloud edit wins.
- The Bluetooth control enables/disables the **Home beacon preference**.
  Advertising still requires primary Home Wi-Fi; Portable/Off-Grid scanning is
  unchanged. The UI separately shows preference, pending delivery and actual advertising.
- Hub telemetry is attempted about once per minute while online, independently
  of collar traffic, in the existing cloud task. Network failure/busy HTTP work
  can delay it; it is not a real-time deadline.
- The cloud UI polls hub status every ten seconds; local UI polls every five.
  Hub and collar cards share the ten-minute stale/dimmed styling. Hub age uses
  the last cloud self-report or successful local API response; failed polls do
  not refresh it. GPS fix age remains separate from contact age.

## Shared device presentation

The cloud `HubCard` is a settings/data adapter around `DeviceCard`, not a separate
card layout. Hubs participate in the same drag ordering, pin-to-top, four-card
expansion limit, avatar expansion, Jump, Follow and Trail controls. The local
dashboard likewise routes hub data through its existing `updateDevice`,
`renderDeviceCard` and marker/popup functions.

Hub cards show their communications mode, Wi-Fi signal, last contact, coordinates,
GPS fix age/time and Home beacon state. Collar-only power profile, battery,
command receive indicator and collar commands are omitted. Bluetooth preference
and editable name/mode emojis/colour retain the hub-specific persistence path.
Collar message-log endpoints are not queried for hubs; hub trails currently build
from observed live fixes in the browser session, not collar history.

Negative browser-only hub keys avoid collisions with collar IDs. They never
change the actual gateway ID or enter collar command/history APIs. A hub without
its own GPS fix keeps its card but has no map marker and a disabled Jump control;
adapter placeholder coordinates must never place it at 0,0.

The shared-card refactor requires the web deployment and updated hub public
assets only, with no new database migration or ingestion deployment. Preserve
existing hub journals/config when updating those assets.

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
  "applied_revision": 0
}
```

Valid modes: home, portable, off_grid. Coordinates must be a valid pair or both
null. When present, fix_age_s is an integer 0–604800. RSSI can be null.
Gateway identity is four hex digits, nonzero and a multiple of 16.
The handler hashes the bearer, scopes it to that enabled gateway and resolves
Family from the database. Browser roles cannot write telemetry or call ingestion RPCs.
Successful response: HTTP 200, accepted, received_at and settings with revision,
ble_enabled, display_name, home_emoji, portable_emoji, marker_colour.
Self reports do **not** claim collar commands or enter collar history.

Local endpoints: GET /api/hub-presence; POST /api/hub-preferences
(display_name, home_emoji, portable_emoji, marker_colour, ble_enabled).
Names/emoji are bounded to 64 UTF-8 bytes in firmware/storage.

## Collar 💡

Both dashboards show only 💡 beside signal quality during the remaining
ten-second receive opportunity. Tooltip/accessible label explain the timer.
Cloud timing uses original live reception, not the collar's GNSS clock.
Historical replay, repeated snapshots and duplicate uploads do not renew it.
Missing trustworthy LoRa reception time remains conservative. Internet latency
can shorten or consume the indicator; it is not a promise of command delivery.
Local timing remains monotonic and radio-driven.

## Rollout and verification

After merging and pulling main, from the repository root:

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
Hardware acceptance: confirm ~one-minute [HUB SELF] 200 responses, genuine GNSS
fix/age, mode icons, reboot-persistent appearance, BLE gate, command priority, and
💡 extinction without historical replay relighting it.
