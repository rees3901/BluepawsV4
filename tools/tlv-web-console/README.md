# Bluepaws TLV Web Console

Local-only support and diagnostic web console for building Bluepaws TLV v1.2 packets, including explicit source/destination addressing, wrapping them for HTTPS ingestion, and sending test telemetry to the Supabase Edge Function.

This is deliberately separate from the customer-facing Vercel app. It is intended to run on a trusted local development/support machine.

## Run

From the repository root:

```powershell
cd .\tools\tlv-web-console
npm start
```

Then open:

```text
http://127.0.0.1:8787
```

Optional port override:

```powershell
$env:BLUEPAWS_TLV_CONSOLE_PORT = "8788"
npm start
```

## Test

```powershell
cd .\tools\tlv-web-console
npm test
```

## Standalone packet reader and builder

Open **4. Packet reader & builder**, or go directly to
`http://127.0.0.1:8787/#workbench`. Restart an older running server after updating
the code, then reload the page to pick up the new endpoints and tab.

This tool has its own inputs and results. It does not edit the simulated fleet,
advance sequences, change wrapper settings, start or stop a run, or send packets.
It works even with an empty credential bundle. Existing simulation runs continue
independently when switching tabs.

- **Read:** paste one complete TLV v1.2 packet (40–64 bytes, including its tag) as
  compact/spaced hex, `0xNN` byte lists, standard padded base64, a collar
  `[PKT] N bytes: ...` line, or a sniffer `[RX] Hex: ...` line. The report shows
  addressing, timestamp, status/profile, flags, GPS quality, battery, optional
  TLVs and authentication. Unknown TLV values remain visible as hex.
- **Build:** edit source/destination IDs (decimal or `0x` hex), Home/Out/Lost/Error
  status, power profile, reason, sequence, timestamp, position and flags. Optional
  TLVs use explicit type/length/value hex bytes in wire order, up to 24 bytes.
  For example, `04 02 02 01 06 01 02` includes firmware 1.2 and reset diagnostic 2.
  Source IDs may identify collars or hubs; destinations may also be cloud `0`
  or broadcast `65535`. Selecting Home does not automatically change flags.
- **Edit a capture:** choose **Load parsed fields into builder** after parsing.
  This preserves known/unknown TLV order and unknown GPS sentinels. Rebuilding
  calculates a new tag with the selected key; it never reuses an unverified tag.
  Reserved/invalid header fields must be corrected before building.
- **Copy:** both operations produce packet hex and base64, plus decoded JSON.

With **No key**, parsing is explicitly unverified and building uses an eight-byte
zero tag marked **unsigned diagnostic**; it is not ready for ingestion. Choose a
custom 32-byte HMAC key (hex or base64), or read the matching source collar's key
from the already-loaded bundle, to sign or verify. Loaded-key lookup is read only;
custom keys are not persisted or returned in the report. A verified HMAC alone
does not prove that production ingestion will accept the packet. Reset diagnostics
and GPS flags are shown as evidence, not inferred brownout/boot-loop diagnoses.

The tool uses only `/api/workbench/meta`, `/api/workbench/parse` and
`/api/workbench/build`; it does not call the simulator build or send routes.

## Credential file

By default the console loads:

```text
tools/devices.json
```

The file is gitignored because it contains plaintext bearer tokens and HMAC keys.

The browser UI can import another JSON bundle, generate new test devices/gateways, save the active bundle back to disk, and generate provisioning SQL for Supabase.

## Security note

The local Node server performs HMAC generation and sends HTTPS requests. This keeps secrets out of the production web app and avoids adding any customer-facing diagnostic routes.
