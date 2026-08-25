# Bluepaws V4 Environment Matrix

This document defines the intended separation between development/test and production environments.

| Area | Development / Test | Production |
|---|---|---|
| Supabase project | Existing `BluepawsV4` | New clean project |
| Supabase project ref | `ykcdaonkvwemedotdpdr` | To be assigned |
| Region | `eu-west-2` | Prefer `eu-west-2` unless requirements change |
| Purpose | Development, integration testing, simulators, test users, experimental data | Real customers, real devices, operational telemetry |
| Git repository | `rees3901/BluepawsV4` | Same repository |
| Git branch model | Feature branches and pull requests | `main` for production deployment |
| Vercel | Preview / development deployments | Production deployment |
| Supabase URL | DEV project URL | PROD project URL |
| Supabase publishable key | DEV publishable key | PROD publishable key |
| Server-side secrets | DEV-only values | Independently provisioned PROD values |
| Database schema | May contain in-progress migrations | Applied from reviewed version-controlled migrations |
| Dummy telemetry | Allowed | Prohibited |
| Simulators | Allowed and should default here | Prohibited by default |
| Test accounts | Allowed | Only deliberate smoke-test/support accounts |
| Real customer accounts | Prohibited | Allowed |
| Real customer telemetry | Prohibited unless specifically authorised for troubleshooting | Allowed |
| Device HMAC keys | Test credentials | Production-generated credentials |
| Device ingest credentials | Test credentials | Production-generated credentials |
| Gateway credentials | Test credentials | Production-generated credentials |
| Storage objects | Disposable test assets | Customer/production assets only |
| Auth configuration | May be relaxed for development where justified | Production-hardened configuration |
| RLS | Must still be enabled and tested | Mandatory and reviewed |
| Edge Functions | DEV deployments | Separate PROD deployments |
| Logs | Test/debug data may be verbose | Minimise sensitive data and production noise |
| Database resets | Permitted when required | Not permitted as normal development practice |
| Data retention | Disposable unless needed for testing | Governed by product and privacy requirements |
| Backups | Useful but not authoritative | Required according to production recovery policy |

## Environment Variable Principle

The application code should not contain environment-specific logic such as hard-coded project URLs or secret credentials.

Vercel should provide environment-specific values, for example:

```text
Production:
NEXT_PUBLIC_SUPABASE_URL=<PROD URL>
NEXT_PUBLIC_SUPABASE_PUBLISHABLE_KEY=<PROD publishable key>

Preview / Development:
NEXT_PUBLIC_SUPABASE_URL=https://ykcdaonkvwemedotdpdr.supabase.co
NEXT_PUBLIC_SUPABASE_PUBLISHABLE_KEY=<DEV publishable key>
```

Only browser-safe values may use the `NEXT_PUBLIC_` prefix.

## Data Movement Rules

Permitted:

- schema and migration promotion from DEV workflow to PROD
- reviewed application and firmware code promotion
- synthetic seed data specifically designed for PROD bootstrap, where required
- anonymised production data into DEV when there is a justified troubleshooting need

Not permitted by default:

- copying the DEV database into PROD
- copying simulator telemetry into PROD
- copying test users into PROD
- copying DEV auth sessions into PROD
- reusing development HMAC/device/gateway secrets in PROD
- routinely copying identifiable customer production data into DEV

## Naming Convention

Recommended human-readable names:

- `BluepawsV4-DEV`
- `BluepawsV4-PROD`

The current Supabase project can be renamed to make its DEV role obvious once convenient.

## Ownership of Truth

For environment decisions, this repository documentation is authoritative.

For schema state, version-controlled Supabase migrations are authoritative.

For secrets, the relevant deployment platform or secret store is authoritative. Secrets must not be documented in Markdown or committed to Git.
