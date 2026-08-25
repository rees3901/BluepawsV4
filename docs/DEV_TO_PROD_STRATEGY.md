# Bluepaws V4 Development-to-Production Strategy

## Purpose

This document records the agreed environment strategy for moving Bluepaws V4 from active development into production without carrying test data, dummy accounts, simulator traffic, stale credentials, or experimental configuration into the production service.

## Core Decision

The current Supabase project is the permanent development and test environment.

Production will use a separate, clean Supabase project created from the version-controlled migration history in this repository.

The same GitHub repository and Vercel project will serve both environments. Environment-specific configuration will determine which Supabase project is used.

Test and simulator data must never be copied into production.

## Environment Model

```text
                    GitHub
             rees3901/BluepawsV4
                       |
              +--------+--------+
              |                 |
         Development        Production
              |                 |
      Supabase DEV        Supabase PROD
      Current project      New clean project
              |                 |
         Vercel Preview     Vercel Production
```

## Current Development Environment

The existing Supabase project is classified as DEV/Test:

- Project name: `BluepawsV4`
- Project ref: `ykcdaonkvwemedotdpdr`
- Region: `eu-west-2`
- Purpose: development, simulator traffic, schema changes, integration testing, test accounts, test devices and experimental data

This project may continue to contain disposable or synthetic data.

## Production Environment

A new Supabase project will be created for production.

The production project must be built from repository-controlled migrations rather than by copying the DEV database.

Production should initially contain:

- schema
- tables
- indexes
- constraints
- triggers
- functions
- RLS policies
- storage configuration
- deployed Edge Functions
- production-specific secrets and credentials

Production must not initially contain:

- historical simulator telemetry
- dummy GPS tracks
- test households
- test invitations
- test users, except deliberately created smoke-test accounts
- test device credentials
- development HMAC keys
- stale auth sessions

## GitHub Strategy

Bluepaws V4 will continue to use one canonical repository:

`rees3901/BluepawsV4`

Do not create separate development and production repositories.

The repository is the source of truth for:

- application code
- firmware
- Supabase migrations
- Supabase Edge Functions
- tests
- deployment documentation

Schema changes intended for production must be represented in version-controlled migrations.

## Vercel Strategy

Continue using the existing Vercel project for the web application.

Target model:

```text
Production deployment / main
        -> PROD Supabase

Preview deployments / feature branches / pull requests
        -> DEV Supabase
```

Environment-specific Supabase URLs and publishable keys should be provided through Vercel environment variables.

The committed `.env` files must never contain server-side secrets, service-role keys, database passwords, device HMAC secrets, ingest credentials, or other private production credentials.

## Supabase Promotion Process

When production is created:

1. Confirm the migration history accurately reproduces the intended schema.
2. Run Supabase security and performance advisors against DEV.
3. Resolve production-blocking security findings.
4. Create the clean PROD project.
5. Apply the full migration chain to PROD.
6. Deploy required Edge Functions to PROD.
7. Configure production Auth settings.
8. Configure production Storage buckets and policies.
9. Create production secrets independently of DEV.
10. Configure Vercel Production environment variables to reference PROD.
11. Create one deliberate production smoke-test user and test device if required.
12. Run end-to-end smoke tests.
13. Only then allow real customer onboarding.

## Secrets and Device Credentials

Production secrets must be treated as independent credentials.

Do not blindly copy DEV values into PROD for:

- device HMAC keys
- device ingest credentials
- gateway credentials
- service-role or secret API keys
- SMTP/API credentials
- webhook secrets
- third-party service credentials

Production device credentials should be provisioned deliberately during manufacturing, QA or onboarding.

## Simulator Safety Rule

Bluepaws simulators and bulk test tools must target DEV by default.

Any ability to target PROD must require an explicit, deliberate override. Ideally production simulator access should be prevented completely unless a controlled production test requires it.

The objective is to make accidental simulator traffic into the production database difficult rather than relying only on operator memory.

## Production Data Rule

Once real customer data exists in PROD:

- DEV data may be deleted or regenerated freely.
- PROD data must never be reset as part of normal development.
- test migrations should be validated in DEV before application to PROD.
- production data should not be copied back into DEV unless it has been appropriately anonymised and there is a specific need.

## Security Review Before Launch

Before production cutover, explicitly review:

- all RLS policies
- all exposed tables and views
- all `SECURITY DEFINER` functions
- Edge Function authentication
- public/anonymous RPC access
- Auth password policy and leaked-password protection
- Storage bucket policies
- service-role key handling
- device/gateway credential storage
- logging for accidental secret disclosure

Current development findings should be treated as items to review before PROD is established, not as justification to copy the current database into production.

## Decision Record

Decision date: 25 August 2026

Decision:

> The existing BluepawsV4 Supabase project remains DEV/Test. Production will be a new clean Supabase project reconstructed from version-controlled migrations. One GitHub repository and one Vercel application project will support both environments through environment-specific configuration. Development and simulator data will not be migrated to production.
