# Bluepaws Web

Customer-facing Next.js dashboard for Vercel. This is a one-for-one React and
TypeScript refactor of the ESP32-hosted interface in `hub/data/`; the original
embedded GUI remains intact.

[![Deploy with Vercel](https://vercel.com/button)](https://vercel.com/new/clone?repository-url=https%3A%2F%2Fgithub.com%2Frees3901%2FBluepawsV4&root-directory=web)

## Local development

```bash
npm install
npm run dev
```

Open <http://localhost:3000>. The dashboard starts in **Live Mode** with no
sample animals or generated telemetry. Open Settings and enable **Tutorial
Mode** to run the typed in-browser simulator with the five reference animals
and a short guided product tour. On a customer's first visit, a compact,
non-blocking card in the bottom-right offers to start the tutorial; choosing
**Not now** is remembered, and the Tutorial Mode control remains available at
the bottom of Settings.

## Verification

```bash
npm run typecheck
npm run lint
npm run build
```

## Vercel

Import the repository into Vercel and set **Root Directory** to `web`. This is
required because the repository root contains firmware rather than a Node.js
application. Use these settings:

- Framework Preset: **Next.js**
- Root Directory: **web**
- Install Command: leave at the framework default
- Build Command: leave at the framework default (`next build`)
- Output Directory: leave at the framework default (`.next`)

The app is self-contained inside `web/`; its build does not read files from the
firmware directories. Live Mode reads the latest provisioned-device positions
from Supabase at request time. Tutorial Mode remains a separate, opt-in,
locally persisted data source, so synthetic records cannot mix with live data.
Its seven-step spotlight tour can be skipped, completed, or replayed from
Settings.

## Current data path

```text
Hub/collar service
    -> authenticated HTTPS POST
    -> Supabase Edge Function (validation and normalization)
    -> Postgres positions table
    -> latest_positions view
    -> web/src/lib/liveTelemetry.ts
    -> dashboard components
```

The Supabase URL and publishable key are public browser configuration and have
safe production defaults in `.env.production`; Vercel environment variables can
override them. The service-role key and per-device bearer tokens must never be
added to `NEXT_PUBLIC_` variables or committed.

Before real customer location data is ingested:

- add Supabase Auth to the customer dashboard;
- replace the temporary anonymous read policy with customer/device-scoped RLS;
- add Realtime or polling so an open dashboard updates without a page refresh.

The expected public environment variables are documented in `.env.example`.
