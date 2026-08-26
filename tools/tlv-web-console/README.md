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

## Credential file

By default the console loads:

```text
tools/devices.json
```

The file is gitignored because it contains plaintext bearer tokens and HMAC keys.

The browser UI can import another JSON bundle, generate new test devices/gateways, save the active bundle back to disk, and generate provisioning SQL for Supabase.

## Security note

The local Node server performs HMAC generation and sends HTTPS requests. This keeps secrets out of the production web app and avoids adding any customer-facing diagnostic routes.
