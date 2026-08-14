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
firmware directories. Live Mode requires Supabase Auth and reads only the
signed-in user's household. Tutorial Mode remains a separate, opt-in, locally
persisted data source, so synthetic records cannot mix with live data. Its
seven-step spotlight tour can be skipped, completed, or replayed from Settings.

## Current data path

```text
Hub/collar service
    -> authenticated HTTPS POST
    -> Supabase Edge Function (validation and normalization)
    -> append-only Postgres positions table
    -> maintained device_latest_positions table
    -> versioned private household:<uuid>:v<access-version> Realtime Broadcast channel
    -> authenticated Next.js dashboard
    -> dashboard components (4G/RF transport badge beside signal quality)
```

The Supabase URL and publishable key are public browser configuration and have
safe production defaults in `.env.production`; Vercel environment variables can
override them. `NEXT_PUBLIC_SITE_URL` identifies the one canonical production
origin used by Auth; the stable Vercel aliases redirect there before an OAuth
flow can begin. The service-role key and per-device bearer tokens must never be
added to `NEXT_PUBLIC_` variables or committed.

### Authentication launch checklist

- Set the Supabase Site URL and allowed redirect URLs for production and Vercel previews.
- Enable Google Auth and configure its OAuth client. Email OTP or magic-link login
  remains available as a fallback.
- Change the email template to include `{{ .Token }}` for six-digit OTP entry,
  then configure production SMTP, CAPTCHA, and appropriate Auth rate limits.
- New accounts create their own named Family during onboarding. Test devices
  `1001`–`1005` remain explicitly assigned to the test Family rather than being
  attached to whichever account signs in first.
- Validate private Realtime delivery, then apply
  `20260809220003_cut_over_private_telemetry.sql` to remove the legacy anonymous
  surface and activate seven-day retention.

### Family accounts and invitations

The customer-facing word **Family** maps to the existing `households` tenancy
boundary in Postgres; database table names remain stable. A person may belong to
more than one Family and chooses an active Family before loading telemetry.

- **Owner**: normal tracking access plus permanent member invitations and future
  billing controls.
- **Member**: normal access to the Family's pets, positions, map and trails, with
  no invitation or billing administration.

One authenticated person can be an Owner in one Family and a Member in another.
The dashboard deliberately loads one selected active Family at a time. Family
membership does not confer billing access: `family_billing_accounts` records
the billing owner separately, and `/account` returns only records owned by the
signed-in person.

An Owner can remove an accepted Member without deleting that person's profile,
login, or other Family memberships. Removal also rotates the Family's private
Realtime channel version. Future telemetry stops going to the old channel;
remaining Members reconnect to the new authorized topic while the removed
person cannot join it.

Owners create an email-bound, one-time link on `/family`. The raw 256-bit token
is returned only when the invitation is created; Postgres stores only its
SHA-256 hash. Opening the link moves it into a seven-day, `HttpOnly`,
`SameSite=Lax` cookie, and the signed-in verified email must match before the
invitation can even be previewed. The authenticated `send-family-invitation`
Edge Function emails the link through Resend, while Copy, browser Share,
WhatsApp, SMS and the user's email client remain available as fallbacks.

Automatic delivery requires a verified Resend sending domain and a
sending-only API key. Store these values only in Supabase Edge Function
secrets; never add them to Vercel or a `NEXT_PUBLIC_` variable:

```powershell
$env:BLUEPAWS_RESEND_KEY = Read-Host "Paste the Resend sending-only API key"
npx --yes supabase@latest secrets set --project-ref ykcdaonkvwemedotdpdr `
  "RESEND_API_KEY=$env:BLUEPAWS_RESEND_KEY" `
  "BLUEPAWS_EMAIL_FROM=Bluepaws <invites@your-verified-domain.example>" `
  "BLUEPAWS_SITE_URL=https://bluepaws-v4-web.vercel.app"
Remove-Item Env:BLUEPAWS_RESEND_KEY

npx --yes supabase@latest functions deploy send-family-invitation `
  --project-ref ykcdaonkvwemedotdpdr --use-api
```

The Function keeps JWT verification enabled, rechecks Owner access through
RLS, derives the recipient and Family name from Postgres rather than the
request, verifies the raw token against its stored hash, rate-limits new
Family invitation emails, and uses the invitation ID as the provider's
idempotency key. If delivery is unavailable, the invitation remains valid and
the interface clearly offers the manual sharing options.

Before deploying the Family settings application code, apply its database
migration from the repository root:

```powershell
npx --yes supabase@latest db push --linked --dry-run
npx --yes supabase@latest db push --linked
```

### Realtime recovery and load testing

Healthy dashboards do not poll. They fetch an authoritative snapshot at page
load, subscription/reconnect, and tab return. Polling starts at 30 seconds only
while Realtime is degraded and backs off to 120 seconds.

After obtaining a short-lived test-user access token, run the connection harness
against a non-production household:

```powershell
$env:SUPABASE_URL='https://project-ref.supabase.co'
$env:SUPABASE_PUBLISHABLE_KEY='sb_publishable_...'
$env:SUPABASE_ACCESS_TOKEN='short-lived-user-jwt'
$env:HOUSEHOLD_ID='household-uuid'
$env:HOUSEHOLD_ACCESS_VERSION='1'
npm run loadtest:realtime -- --clients 10 --duration 60
```

Repeat at 100, 500, and 1,000 clients only after the Supabase Realtime quota is
raised appropriately. Never commit an access token.

The expected public environment variables are documented in `.env.example`.
