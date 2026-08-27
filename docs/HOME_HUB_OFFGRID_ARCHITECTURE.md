# Home Hub Off-Grid Runtime

Status: implementation baseline for the Heltec Wireless Tracker V2 Home Hub testbed.

## Purpose and limits

Off-Grid mode keeps collar finding usable when the Home Hub cannot reach the cloud. The hub remains an always-on FreeRTOS device, receives raw TLV v1.2 LoRa packets, serves a local dashboard, queues addressed commands, and replays stored packets when connectivity returns.

The initial bounded design supports:

- 16 collars.
- Eight simultaneous Server-Sent Events clients; later clients use 10-second polling.
- The latest 100 complete packets per collar.
- A fixed 10-minute stale threshold.
- An open `Bluepaws Hub 0010` hotspot and captive portal.
- An optional four-digit, RAM-only command PIN. Viewing remains open.

Account, Family, invitation and billing controls are deliberately absent.

## Trust model

The Home Hub does not store collar HMAC keys. A newly received offline packet is therefore displayed as **Locally received — verification pending**. Cloud-validated cached packets are marked validated. A permanently rejected replay record is quarantined and excluded from authoritative trails.

The optional PIN protects commands, not locations. It is erased on reboot and whenever the hub leaves Off-Grid mode. Unlock tokens are random, RAM-only, tied to the browser IP address, and limited to eight sessions. Five failed attempts impose a one-minute delay.

Only explicitly allowlisted public assets are served from LittleFS. Configuration,
credentials and raw journal files are never downloadable by path. Wi-Fi/cloud
configuration changes require locally enabled provisioning and are blocked in
Off-Grid mode; the public settings modal omits those controls otherwise.

## Runtime separation

- LoRa task: highest-priority radio reception and command transmission.
- Storage task: asynchronous journal writes; radio reception never waits on flash.
- Web task: local HTTP API, static assets, SSE and captive DNS.
- Cloud task: live forwarding and bounded offline replay.
- BLE task: Home beacon or portable/off-grid scanning.
- Time task responsibilities: GNSS first when available, NTP second, then monotonic persisted fallback.

LittleFS stores fixed-size CRC32-protected records in one circular journal file per collar. A corrupt or partial record is ignored during reconstruction. The storage interface is isolated so an SD implementation can later add regional tiles and larger history.

## Local endpoints

- `GET /api/devices` — latest reconstructed collar state.
- `GET /api/history?device=<id>&limit=<1..100>` — bounded local history.
- `GET /api/history.csv?device=<id>` — per-collar export.
- `GET /events` — telemetry, command state and verification SSE.
- `POST /api/command`, `/api/find`, `/api/device-status` — addressed TLV v1.2 commands.
- `GET /api/security`, `POST /api/security/pin`, `POST /api/security/unlock` — local command guard.
- `POST /api/hub-mode` — confirmed mode switch; collar state is unchanged.
- `GET /tiles/{z}/{x}/{y}` — map-source abstraction. The first build returns the bundled vector skeleton rather than network tiles.

Android, Apple and Windows connectivity-check paths redirect to `/`. Wildcard DNS resolves to `192.168.4.1` while the Off-Grid AP is active.

## Offline replay

The journal retains the original TLV, local ID, hub reception time, RSSI/SNR and verification state. Pending records are replayed oldest first to `ingest-position-batch`:

- Maximum 10 records and 16 KB per request.
- Accepted and duplicate records become validated.
- Permanent rejections are quarantined.
- Transient failures remain pending with exponential backoff up to five minutes.
- Historical replay never claims a pending collar command.

Database `received_at` remains the cloud upload time. `effective_seen_at` records the original hub reception time, so replayed history cannot make an old collar look newly present. Existing recorded-time ordering prevents old coordinates from replacing a newer current position.

## Gateway snapshot

`hub-snapshot` authenticates a gateway bearer token and returns only that gateway's Family. It supplies provisioned devices, emoji/colour metadata, current positions first, and observation history using bounded cursor pagination. Photograph objects are intentionally excluded from the initial hub cache.

The current firmware imports appearance metadata only. It reconstructs positions
and trails from packets it has received and journalled itself; importing the
snapshot's cloud position/history pages is still pending. Do not assume a freshly
flashed hub already has the cloud's complete recent history.

`service_role` requires read-only grants on `observations` and `observation_paths`
for this endpoint; no extra browser/public grants are needed. Snapshot failures
include a safe stage/code and request ID, never credential values.

## Deployment

After review and merge:

```powershell
npx --yes supabase@latest db push --linked --dry-run
npx --yes supabase@latest db push --linked
npx --yes supabase@latest functions deploy ingest-position-batch --project-ref ykcdaonkvwemedotdpdr --no-verify-jwt
npx --yes supabase@latest functions deploy hub-snapshot --project-ref ykcdaonkvwemedotdpdr --no-verify-jwt
```

Build and flash both the firmware and bundled filesystem:

**Back up an existing hub first.** `uploadfs` replaces the entire LittleFS
partition, including saved Wi-Fi/gateway credentials, metadata and journals.
For an already configured hub, preserve those private files in a local, ignored
staging image with the new public assets. Never commit that image or its secrets.
The plain `uploadfs` command below is for a fresh/reprovisioned hub only.

```powershell
py -3.11 -m platformio run -e hub
py -3.11 -m platformio run -e hub -t upload --upload-port COM7
py -3.11 -m platformio run -e hub -t uploadfs --upload-port COM7
```

Do not omit `uploadfs`: Leaflet, the application, styles and coarse basemap are served from LittleFS with no CDN dependency.

## Bench verification — 27 August 2026

COM7 firmware and local assets flashed with verified hashes after an 8 MB flash
backup. Wi-Fi and gateway settings were retained. Verified on the physical hub:

- Home Wi-Fi reconnect, NTP sync, snapshot HTTP 200 and five cached appearances.
- Local dashboard/Leaflet/basemap HTTP 200; settings hide provisioning fields.
- Private configuration/journal paths return 404; non-provisioning config POST
  returns 403, including in Off-Grid mode.
- Form-encoded Home/Off-Grid switching, AP enabled in Off-Grid, captive-check
  routes returning the page, and explicit confirmation before leaving Off-Grid.
- Optional PIN blocks unauthorised commands/disable requests, accepts a correct
  unlock, and clears PIN/sessions when returning Home. Empty command bodies were
  used so the access test did not queue any radio commands.
- Eight SSE connections receive heartbeats; a ninth returns 503. About 95 KB
  free heap remained during this short bench test. This is not a load soak.
- Four local regression checks, Deno type check, live SQL permission assertions
  and database lint pass. Existing search-party RPC/password-policy advisor
  warnings remain separate from these changes.

The hub was left in Home mode with its hotspot off. Phone OS captive discovery,
eight separate Wi-Fi stations, long-running LoRa/flash concurrency, 101-packet
retention/reboot, full replay validation, and cloud position/history import
still need acceptance testing or implementation. COM23 was occupied by another
program, so no forced collar transmission was attempted in this deployment.
