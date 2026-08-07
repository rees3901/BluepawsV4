# Bluepaws Web

Customer-facing Next.js dashboard for Vercel. This is a one-for-one React and
TypeScript refactor of the ESP32-hosted interface in `hub/data/`; the original
embedded GUI remains intact.

## Local development

```bash
npm install
npm run dev
```

Open <http://localhost:3000>. The current data source is a typed in-browser
mock that simulates the five reference animals and emits updates every two
seconds.

## Verification

```bash
npm run typecheck
npm run lint
npm run build
```

## Vercel

Import the repository into Vercel and set **Root Directory** to `web`. Next.js
is auto-detected; no custom build command or output directory is required.

## Planned data path

```text
Hub/collar service
    -> authenticated HTTPS POST
    -> Supabase Edge Function (validation and normalization)
    -> Postgres telemetry tables
    -> Supabase Realtime
    -> web/src/lib/telemetry.ts
    -> dashboard components
```

`src/lib/telemetry.ts` is the only data-source selection point. A future
Supabase adapter will implement the existing `TelemetrySource` interface, so
database integration does not require UI rewrites.

When that phase starts:

- only the Supabase URL and publishable key may use `NEXT_PUBLIC_` variables;
- secret or service-role keys must stay inside the Edge Function/server;
- exposed tables must have Row Level Security and customer-scoped policies;
- the Edge Function must authenticate devices, validate payloads, reject
  replays, and use idempotency keys such as `(device_id, msg_seq)`.

The expected public environment variables are documented in `.env.example`.
