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
- Network task (core 0, priority 2): sole owner of Wi-Fi role changes, connection
  recovery, idle discovery and captive DNS; never waits for NTP, TLS or flash.
- Web task (core 1, priority 2): local HTTP API, static assets and SSE, below LoRa
  (core 1, priority 3). Cloud runs at priority 1 on core 0. ESP-IDF's own Wi-Fi
  and TCP/IP tasks retain their built-in priorities; do not raise application
  tasks above them.
- Cloud task: live forwarding and bounded offline replay.
- BLE task: Home beacon or portable/off-grid scanning.
- Time task responsibilities: GNSS first when available, NTP second, then monotonic persisted fallback.

LittleFS stores fixed-size CRC32-protected records in one circular journal file per collar. A corrupt or partial record is ignored during reconstruction. The storage interface is isolated so an SD implementation can later add regional tiles and larger history.

## Automatic Wi-Fi recovery (27 August 2026 amendment)

This supersedes the earlier manual-only Off-Grid entry rule:

1. Boot, or loss of the connected STA uplink, starts one **30-second total**
   recovery window. LoRa reception continues; the Home BLE beacon pauses when
   primary Wi-Fi is lost.
2. Try primary for 15 seconds, then a configured secondary for the remainder.
   If only one distinct SSID is configured it receives the full 30 seconds.
   Primary success selects Home; secondary success selects Portable.
3. If neither connects, automatically enter Off-Grid: open `Bluepaws Hub 0010`,
   channel 6, up to eight stations, GUI at `http://192.168.4.1`, wildcard DNS and
   captive-check routes. A failed AP/DNS start is logged and retried every five
   seconds; a healthy AP is not repeatedly recreated.
4. Off-Grid is latched. Do not reconnect automatically or move the AP onto a
   router's channel while someone is searching. Home/Portable exit requires
   confirmation; collar power profiles and queued collar commands are unchanged.
   An explicitly selected Portable retry tries secondary first, then primary.
   Failure returns to Off-Grid after the same 30-second total budget.

AP+STA uses one physical radio. Keep `WIFI_PS_MIN_MODEM` enabled: this ESP32
Wi-Fi/Bluetooth coexistence stack aborts if modem sleep is disabled. This does
not put the hub CPU or its application tasks to sleep. STA auto-reconnect is
disabled; only the network task calls `WiFi.begin`.
Off-Grid scans for known SSIDs at most once per minute **with zero associated AP
stations**, asynchronously, with short channel dwell. A joining station cancels
an active scan. While anyone is connected, scans are suspended and the last
discovery hint may be stale. Discovery does not prove internet access or change
modes. The user may explicitly choose Home/Portable to retry at any time.
BLE find scanning is passive at 20% scan duty in short cycles; BLE role changes
are owned by the BLE task, not HTTP handlers. These mitigations reduce radio
contention; they are not a guarantee against interference or poor power supply.

Wi-Fi association loss (not merely an internet or cloud outage) triggers this
policy. Router disappearance detection itself is handled by the Wi-Fi driver;
the 30 seconds begin when the hub observes it disconnected.

`POST /api/hub-mode` returns **202 `{ "pending": true }`**. Radio changes execute
after the reply has had time to leave; actual mode is reported in status and SSE
heartbeats. Leaving the AP will disconnect the browser: join the selected uplink
and reopen the hub's LAN IP. Off-Grid PIN/sessions are cleared on exit.

### Configure a secondary uplink

In the COM7 serial console, 115200 baud (replace examples locally):

```text
wifi secondary ssid="Your phone hotspot" pass="Your hotspot password"
status
```

This preserves primary Wi-Fi and gateway credentials, saves and restarts.
Omit `pass` for an open secondary network. `wifi secondary clear` removes only
the secondary network. Duplicate primary/secondary names are treated as one
candidate. Provisioning-only web settings expose the same secondary fields;
blank SSID fields preserve saved values and removal is explicit. Credentials
stay private in LittleFS; configuration uses a complete temporary file/rename.

Bench mode controls (no router configuration changes):

```text
mode off_grid
mode home confirm
mode portable confirm
```

Status includes network phase, recovery remaining, AP clients/channel/start
failures, and network/web minimum free stack. Core priorities alone cannot fix
radio-channel contention; see [Espressif Wi-Fi guidance](https://docs.espressif.com/projects/esp-idf/en/release-v5.4/esp32/api-guides/wifi.html).

### Failover acceptance test

- Start Home and record `/api/status`; disable only the hub's primary SSID.
- With a configured reachable phone hotspot, verify Portable within the recovery
  window. With neither reachable, verify Off-Grid/AP about 30 seconds after the
  disconnected/search log (allow driver detection delay).
- Join the open hotspot, browse `http://192.168.4.1`, verify wildcard DNS/captive
  discovery and live LoRa updates. Leave a client connected for at least several
  minutes: no STA reconnect, no background scans, no AP recreation, stable heap.
- Restore the router: connected AP clients must stay connected, Off-Grid stays
  selected. Confirm Home to leave, verify STA/cloud recovery and cleared PIN.
- Repeat with missing credentials, wrong password, secondary loss, eight clients,
  slow HTTP clients, BLE scanning, and power-cycle. Phone OS captive discovery
  and long-duration RF soak remain physical-device tests, not native-test claims.

Native policy tests: compile `hub/tests/wifi_failover_test.cpp` with
`-std=c++17 -Ihub/include`. They cover the 30-second boundary, primary/secondary
success, outage restart, absent credentials, confirmed retry policy and millis
wraparound. `py -3.11 -m unittest tools.test_hub_public_assets` checks ownership,
configuration privacy and asynchronous mode API wiring.

## Local endpoints

- `GET /api/devices` — latest reconstructed collar state.
- `GET /api/history?device=<id>&limit=<1..100>` — bounded local history.
- `GET /api/history.csv?device=<id>` — per-collar export.
- `GET /events` — telemetry, command state and verification SSE.
- `POST /api/command`, `/api/find`, `/api/device-status` — addressed TLV v1.2 commands.
- `GET /api/security`, `POST /api/security/pin`, `POST /api/security/unlock` — local command guard.
- `POST /api/hub-mode` — confirmed mode switch; collar state is unchanged.
- `GET /tiles/{z}/{x}/{y}` — map-source abstraction. The first build returns the bundled vector skeleton rather than network tiles.

Android, Apple and Windows connectivity-check paths redirect AP-side HTTP clients
to the absolute `http://192.168.4.1/` address. This includes Windows' `/redirect`
and `/fwlink` browser-launch paths. Wildcard DNS resolves to `192.168.4.1` while
the Off-Grid AP is active. A request for `/` on a captured foreign hostname also
redirects to the IP, keeping assets, SSE and API requests on the hub origin.
Missing canonical-IP files and APIs still return 404; no private file-serving
permissions are added. LAN requests do not activate the captive redirect.

The operating system decides whether to open a portal. Ethernet, VPNs, cellular
fallback, cached public DNS or secure DNS can bypass the hub. The firmware cannot
redirect traffic which never reaches it, or intercept HTTPS without certificate
errors. Keep `http://192.168.4.1/` as the manual fallback; do not disable NCSI or
change users' routing/security settings as a workaround. Microsoft documents
the Windows probe/MSN behaviour [here](https://learn.microsoft.com/en-us/troubleshoot/windows-client/networking/internet-explorer-edge-open-connect-corporate-public-network).

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

## Earlier Off-Grid baseline verification — 27 August 2026

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

## Automatic failover hardware verification — 27 August 2026

The automatic-failover firmware and updated public assets were flashed to COM7
after a complete 8 MB backup, preserving private Wi-Fi/gateway configuration.
Two startup issues found on hardware were fixed: web socket binding now waits
for lwIP initialization, and Wi-Fi retains minimum modem sleep for Bluetooth
coexistence (disabling it aborted this ESP32 firmware).

- A temporary nonexistent primary SSID started recovery at 1.5 seconds after
  the test command; the open AP appeared at 31.5 seconds: **30 seconds of search**.
- A Windows AX210 client associated with `Bluepaws Hub 0010` on channel 6 and
  obtained access to `192.168.4.1`. No router configuration was changed.
- Dashboard, app/CSS, Leaflet and basemap assets returned HTTP 200 over the AP.
  Android/Apple/Windows captive-check routes served the page; a real UDP DNS
  query for an arbitrary hostname resolved to `192.168.4.1`.
- Eight SSE connections stayed registered while 30 status requests completed
  in 10.5 seconds. A ninth SSE connection received 503 for polling fallback.
  Free heap was approximately 86–87 KB, AP stayed on channel 6, and AP start
  failures remained zero. These were eight connections from **one Wi-Fi station**.
- An unconfirmed Home request returned 409. The queued mode API returned 202;
  explicit confirmed Home retry restored primary Wi-Fi.
- The real primary credentials were restored, snapshot returned HTTP 200,
  and the hub was left in Home mode with AP off. The temporary Windows Wi-Fi
  profile was removed and its adapter returned to its original disconnected state.
- Native failover policy tests cover primary/secondary timing, loss after a
  successful connection, one shared deadline, confirmed retry and clock rollover.
  Six Python source regression checks and JavaScript syntax checking pass.

Still unverified on hardware: router switch-off/beacon-loss detection latency,
real secondary-phone-hotspot association, eight separate Wi-Fi stations, phone
OS captive-portal popups, and extended LoRa/flash concurrency under load. No
collar packets arrived during this short AP bench run; radio initialization
remained active, but that alone does not establish loss-free concurrent reception.
No cloud schema, Edge Function or Vercel deployment is required for this change.

## Populated-card and captive redirect correction

The earlier empty-page test missed a rendering error: Portable/Off-Grid cards
looked up BLE proximity using an undefined `id`, rather than `dev.id`. Home mode
short-circuited that expression. The correction is covered by executable Node
tests of the actual renderer for Home, Portable, Off-Grid, no-GPS presence,
timer refresh, per-device BLE values and expanded GPS cards.

The hub now explicitly handles Windows portal launch routes and serves the same
blue-paw favicon as the cloud dashboard. Main HTML/CSS/JS are sent with no-store
headers so a refreshed browser uses newly flashed assets. Broken SSE sockets
are removed after a failed/partial write instead of repeatedly retrying them.

Verification commands:

```powershell
node --test tools/test_hub_device_cards.mjs
py -3.11 tools/test_hub_public_assets.py
```

Five JavaScript renderer tests, eight Python regression checks and the hub
firmware build pass. The original card crash was reproduced in the browser with
a received device present, and the old `/redirect` response was confirmed as
404. Hardware verification of the corrected assets requires flashing both the
firmware and a filesystem image that preserves current private files/journals.
Do not use an old filesystem backup over more recent received packets.

The separately observed 49-byte LoRa command-reply structural failures are not
fixed by this web change and still require protocol/ACK diagnostics.

## Captive welcome page and browser handoff

Captive probes and unknown captured external hostnames now redirect to
`http://192.168.4.1/welcome`, a small, locally bundled entry page. The full
dashboard remains at `http://192.168.4.1/`. The existing mDNS hostname
`http://bluepaws.local/` is explicitly recognised, including case-insensitive
Host values, the normal `:80` suffix and a DNS trailing dot. It no longer
gets mistaken for an external captive-probe hostname.

The welcome page provides Bluepaws branding, a prominent dashboard link, both
IP and hostname links, and instructions for remaining connected when the OS
closes its temporary sign-in window. It does not pretend that the hotspot has
internet access, force an external browser launch, or redirect on a timer.
The numeric IP remains the primary route because mDNS resolution depends on
the client and its network configuration.

`GET /welcome` and `GET /api/welcome` are AP-side only. The snapshot contains
the hub ID, number of collars heard in the last ten minutes, known collar
count, youngest available report age (null when absent), and clock-sync state.
It contains no credentials, coordinates, names or command interfaces. The
page makes one request at a time, refreshes every 15 seconds while visible,
aborts after five seconds, and uses no SSE connection. Navigation remains
available without JavaScript or when the snapshot fails. Approximate timing
and cached/unverified reports are identified rather than implying cloud
validation. The existing private filesystem/API boundaries remain unchanged.

Android's “Use this network as is” action can close the captive window; a
normal link cannot guarantee that the OS opens a separate browser. Windows
may reach Microsoft's own redirect through another adapter/VPN instead of
the hub. Users should stay on the hub Wi-Fi and open the numeric address in
their normal browser in either case. Do not claim that this page fixes OS
network selection or guarantees automatic captive-window launch.

Verification:

```powershell
node --test tools/test_hub_welcome.mjs tools/test_hub_device_cards.mjs
py -3.11 tools/test_hub_public_assets.py
python -m platformio run -e hub
```

Ten executable JavaScript checks, eleven Python regression checks and the hub
build pass. Browser verification of the actual welcome assets used a local
read-only preview with synthetic hub statistics; no console errors occurred.
Hardware follow-up on 27 August: the hub on COM7 was backed up in full, then
flashed with the firmware and updated filesystem; both write hashes verified.
The saved configuration, appearances and journal were preserved byte-for-byte
in the replacement image. Automatic Off-Grid fallback and incoming LoRa
telemetry were observed after reboot.

On-device HTTP checks passed for Windows/Android/Apple redirects to `/welcome`,
direct-IP and mDNS dashboard/welcome requests (including Host case, port and
trailing-dot variants), private-path/API 404s, favicon assets and the small
welcome snapshot. A real `http://bluepaws.local/` request from Windows returned
the dashboard with HTTP 200 and no redirect. The snapshot reported one recent
collar and approximate clock state; it consumed no SSE slots. The in-app
browser blocked the post-upload reload by URL policy, so the actual on-device
visual/OS captive-window handoff still needs checking on the user's phone.
Future uploads must likewise preserve a fresh copy of private files/history.
