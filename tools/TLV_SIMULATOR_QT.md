# Bluepaws PySide6 TLV desktop console

`tlv_simulator_qt.py` is the primary desktop interface for manually building and
sending Bluepaws v1.1 telemetry. PySide6 replaced CustomTkinter after a populated
230-widget Qt prototype demonstrated normal Windows window movement on the target
development PC. The former `tlv_simulator_gui.py` remains temporarily available
as a fallback, but new desktop work should target this Qt console.

The interface delegates packet construction, authentication, wrapper validation
and HTTP posting to `tlv_packet_codec.py`, which is also used by the VPS simulator.

## Install and run on Windows

From the repository root, use a dedicated environment so Qt cannot conflict with
the existing CustomTkinter console:

```powershell
py -3.11 -m venv .venv-qt
& ".\.venv-qt\Scripts\python.exe" -m pip install --upgrade pip
& ".\.venv-qt\Scripts\python.exe" -m pip install -r ".\tools\requirements-qt-gui.txt"
& ".\.venv-qt\Scripts\python.exe" ".\tools\tlv_simulator_qt.py" --check
& ".\.venv-qt\Scripts\python.exe" ".\tools\tlv_simulator_qt.py"
```

Expected dependency check:

```text
Bluepaws Qt console dependencies available (PySide6 6.11.1).
```

## Safe use

Load the private typed credentials bundle on the packet-builder tab. It contains
separate `devices` and `gateways` arrays; the legacy flat device array remains
supported. Secrets stay masked and are excluded from wrapper previews and logs.
The selected transport automatically uses the selected device bearer for direct
LTE or the selected gateway bearer and GUID16 for a LoRa home-hub relay.

The console supports valid packets and deliberate negative HMAC tests, custom
TLVs, decoded payload and wrapper previews, repeated or advancing sequences and
cancellable background HTTP posting. Request results appear on their own third
tab in a colour-coded summary table with a clear
success, duplicate, authorization, client, server or network status. Selecting a
row shows its complete formatted JSON response for diagnosis. Use test devices
only. Never put a Supabase service-role key in the credential file or bearer field.

Startup defaults favour the simplest successful end-to-end test: the HMAC mode
is **Valid HMAC**, and **Include optional TLVs** is off. This produces the basic
authenticated 40-byte packet. Enabling the global TLV control activates both the
selected v1.1 and custom/unknown TLV editors. Response-table columns have useful
starting widths and remain interactively resizable and movable.

The manual send controls default to 5 packets at 5-second intervals with a
15-second HTTP timeout. In fleet mode this means five update cycles per checked
device, not five requests in total. Devices in one cycle are sent together and
the configured interval is applied between cycles. Cookbook recipes replace the
count and interval with their own test-specific values while active.

## Multi-device fleet mode

Load a typed credentials bundle, enable **Fleet mode**, and check the collars to
include. Every cycle then builds one packet per selected device using that
device's own 32-byte HMAC key. LTE requests also use the corresponding device
bearer; LoRa requests share the selected hub bearer while retaining independent
collar HMACs. The first device keeps the entered starting coordinate and the
remaining markers are fanned out by small 25 m steps so they are visible on the
map immediately.

Each collar maintains independent sequence, movement and measurement state
between successful runs. The response log includes a Device column. **Select
all** and **Select none** make larger bundles manageable. Single-device mode
continues to use the currently selected credential and editable packet fields.

Generate a private five-device bundle and its one-time Supabase provisioning SQL
from the repository root:

```powershell
py -3.11 .\tools\generate_tlv_credentials.py `
  --count 5 `
  --start-device-id 1001 `
  --household-id <TEST-HOUSEHOLD-UUID> `
  --gateway-guid16 0016
```

Both generated files are Git-ignored. Run `tools/vps_devices.provision.sql` once
in the Supabase SQL Editor, then securely delete the SQL file; it contains the
one-time plaintext HMAC values needed to populate Vault. Keep
`tools/vps_devices.json` private for the PySide/VPS simulator and hardware
provisioning. Use a new `--key-version` for deliberate HMAC rotation rather than
rerunning an existing version.

The optional TLV container uses a fully muted background, border, nested panels
and table when unchecked, making its inactive state visually unambiguous.

## Test cookbook

**Use test cookbook** is an opt-in section at the top of the packet-builder tab.
Enabling it applies the selected recipe, fixes its send count and interval, and
leaves the generated packet fields visible for inspection. **Run selected recipe**
starts the sequence and switches directly to the **Send & Response Log** tab so
its results are visible; the existing **Send** button there runs the same prepared
sequence. Disabling the cookbook returns count, interval and live-mode controls to
manual editing while leaving the last applied values visible.

The cookbook includes:

- **Basic sunny day** — 10 valid LTE header-only packets with gentle 50 m steps;
- **Moving pet** — valid traffic with bounded 200 m random-walk steps;
- **Rich known TLVs**, **Maximum TLV budget**, and **Random TLV assortment**;
- **Bad day** — exactly 2 valid and 8 deliberately corrupt HMAC packets;
- **Mixed bag** and **Fully randomized** — bounded combinations of outcomes,
  telemetry and optional TLVs;
- **Duplicate retry storm**, **Sequence rollover**, and **Out-of-order delivery**;
- **LoRa relay sunny day**, **LTE radio fade**, and **HMAC rejection only**.

Random recipes never alter device identity, credentials, endpoint or selected
transport authentication. Negative recipes should be used only with provisioned
test devices and gateways. A corrupt inner HMAC is expected to produce an HTTP
authentication failure; a valid new observation is expected to return `201`.

Packet Base64, packet hex, decoded payload JSON and HTTPS wrapper JSON update
automatically after a short pause whenever an applicable field, selector, flag or
TLV changes. The generated fields are read-only. **Send** also performs an immediate
final rebuild, so a request cannot use an older preview when an edit has just been
made. Invalid input clears the previous generated output instead of leaving stale
data visible.
The second tab uses the full available width for **Payload and packet previews**.
Its first window decodes the transmitted Base64 bytes into human-readable JSON,
including named status/profile/reason values, active flags, position data, decoded
known and unknown TLVs, and local HMAC validation. Its second window shows the
complete HTTPS JSON wrapper without the Authorization header. The wrapper summary
reports both the exact compact UTF-8 JSON request-body size sent by the console and
the decoded TLV packet size. HTTP headers and TLS framing are intentionally excluded
because their wire overhead depends on the connection.

## Live movement simulation

**Live simulation** is enabled by default. For a multi-packet send it:

- advances the 16-bit message sequence for every packet, wrapping after 65535;
- starts packet timestamps from the current Unix time and advances them by the
  configured interval;
- updates the form to the next sequence and latest transmitted location when the
  run finishes;
- optionally applies a bounded random-walk step after the first packet. Enable
  **Random-walk movement** and choose the maximum metres per packet. Movement is
  enabled at startup with a 200 m default and a hard 300 m ceiling;
- adds bounded measurement noise after the first packet: battery ±3 mV, GNSS
  accuracy ±2 m, fix age and satellites ±1, activity score ±2, RSSI/RSRP ±2 dB,
  and SNR/RSRQ/SINR ±0.5 dB;
- advances the optional uptime TLV by the simulated elapsed time. Unknown fix-age
  (`65535`) and satellite-count (`255`) sentinels are never varied.

Fields affected by live mode carry a `↗` advance or `±` variation marker in the
interface. These changes model sensor and radio measurement noise only: device
identity, status, power profile, TX reason, flags, firmware, reset reason,
credentials, transport and HMAC configuration remain exactly as selected.

Cookbook recipes set movement to match their purpose: normal traffic uses gentle
steps, mixed/random stress traffic uses progressively larger bounds up to 300 m,
and duplicate, maximum-TLV, radio-fade and HMAC-rejection recipes remain
stationary so movement cannot obscure the behaviour under test.

Turn live simulation off only when intentionally repeating the exact same packet
to test idempotent duplicate handling. In that mode, a first `201` followed by
`200` duplicate responses is expected.

## Google Maps coordinates

**Open Google Maps** uses Google's documented cross-platform Maps URL and requires
no Maps API key. It opens the current coordinate in the default browser. To pick
a new point, right-click it in Google Maps and copy the displayed coordinates, or
copy the page URL. Return to the console, choose **Paste coordinates**, and paste
either `latitude, longitude` or the full Maps URL. A desktop program cannot safely
read a click from an unrelated Chrome tab directly, so the clipboard is the
explicit handoff between the two applications.

Expected responses include `201` for a new observation, `200` with
`duplicate: true` for an idempotent retry, `400` for malformed input, `401` for
invalid credentials/HMAC, `409` for an identity conflict and `503` for an
ingestion/database-stage failure.
