# Bluepaws V4 Production Readiness Checklist

Use this checklist before the first production launch and again for major production changes.

## 1. Environment Separation

- [ ] Existing Supabase project is explicitly treated as DEV/Test.
- [ ] New clean Supabase PROD project has been created.
- [ ] PROD was built from version-controlled migrations, not cloned from DEV data.
- [ ] Vercel Preview/Development points to DEV Supabase.
- [ ] Vercel Production points to PROD Supabase.
- [ ] No server-side production secrets are committed to Git.
- [ ] Simulators default to DEV.
- [ ] Simulator access to PROD is disabled or requires an explicit controlled override.

## 2. Database and Migrations

- [ ] Full migration chain applies successfully to a clean database.
- [ ] Migration history in Git matches the intended production schema.
- [ ] Required extensions are enabled.
- [ ] Primary keys, foreign keys and uniqueness constraints are reviewed.
- [ ] Critical indexes are present.
- [ ] Deduplication constraints for telemetry ingestion are verified.
- [ ] Triggers and scheduled jobs are documented and tested.
- [ ] No test-only database objects are accidentally included.

## 3. Row-Level Security and API Exposure

- [ ] RLS is enabled on every table exposed through the Supabase Data API.
- [ ] Each RLS policy has been reviewed against the intended ownership model.
- [ ] Authenticated users cannot access another household's devices or telemetry.
- [ ] Anonymous users cannot access customer data.
- [ ] Sensitive credential tables are not readable through normal client roles.
- [ ] Views have been reviewed for RLS behaviour and `security_invoker` requirements.
- [ ] `SECURITY DEFINER` functions have been individually reviewed.
- [ ] Public RPC functions expose only the minimum required capability.
- [ ] Supabase Security Advisor findings have been reviewed and production-blocking findings resolved.

## 4. Authentication

- [ ] Production Auth URL and redirect URLs are correct.
- [ ] Email/password or other enabled providers are configured deliberately.
- [ ] Password strength requirements are suitable for production.
- [ ] Leaked-password protection is enabled if available for the chosen plan/configuration.
- [ ] Test users from DEV are not migrated into PROD.
- [ ] A deliberate smoke-test account exists if needed.
- [ ] Account deletion and session handling behaviour has been tested.

## 5. Device and Gateway Security

- [ ] Production device HMAC keys are generated independently from DEV keys.
- [ ] Production device ingest credentials are generated independently from DEV.
- [ ] Production gateway credentials are generated independently from DEV.
- [ ] Device credentials are never exposed to the browser client.
- [ ] Device provisioning process is documented.
- [ ] Credential rotation/revocation process is documented.
- [ ] Lost, returned or compromised collars can be disabled.
- [ ] Ingestion rejects invalid authentication/HMAC data.
- [ ] Replay/deduplication behaviour has been tested.

## 6. Edge Functions

- [ ] `ingest-position` is deployed to PROD.
- [ ] `send-family-invitation` is deployed to PROD.
- [ ] `send-search-party-link` is deployed to PROD.
- [ ] Required function secrets are configured separately in PROD.
- [ ] JWT verification requirements are reviewed for each function.
- [ ] Functions do not log secrets, raw credentials or unnecessary personal data.
- [ ] Error handling returns safe responses.
- [ ] Rate-limiting/abuse controls have been considered for externally callable endpoints.

## 7. Storage

- [ ] Required production buckets exist.
- [ ] Bucket public/private status is deliberate.
- [ ] Storage RLS policies have been reviewed.
- [ ] DEV files are not copied blindly into PROD.
- [ ] Production file naming and ownership rules are tested.
- [ ] Upload size/type restrictions are appropriate.

## 8. Vercel and Front End

- [ ] Production deployment is sourced from the intended branch.
- [ ] Production environment variables point only to PROD services.
- [ ] Preview environment variables point only to DEV services.
- [ ] No service-role or other private key uses the `NEXT_PUBLIC_` prefix.
- [ ] Custom production domain is configured and HTTPS is valid.
- [ ] Login/logout/session persistence is tested in production configuration.
- [ ] Map and realtime updates work against PROD.
- [ ] Production error handling does not expose sensitive implementation details.

## 9. End-to-End Telemetry Test

Using one deliberate production test collar/device:

- [ ] Device is provisioned with PROD credentials.
- [ ] GNSS/telemetry packet reaches the production ingestion endpoint.
- [ ] HMAC/authentication validates correctly.
- [ ] Observation is stored once.
- [ ] Duplicate retry is deduplicated correctly.
- [ ] Latest-position projection updates correctly.
- [ ] Device appears only to its authorised household.
- [ ] Vercel frontend displays the position.
- [ ] Realtime update is received without manual refresh.
- [ ] LoRa-via-hub and LTE paths are both tested where applicable.
- [ ] Command queue/acknowledgement path is tested where applicable.

## 10. Customer and Household Flows

- [ ] New customer signup works.
- [ ] Household creation works.
- [ ] Collar/device registration works.
- [ ] Multiple pets per household behave correctly.
- [ ] Family invitation flow works.
- [ ] Invitation revocation/expiry works.
- [ ] Search-party share creation works.
- [ ] Search-party public access exposes only intended data.
- [ ] Search-party share revocation works immediately enough for the product requirement.

## 11. Operational Safety

- [ ] A production backup/recovery approach is defined.
- [ ] Restore procedure has been understood or tested.
- [ ] Production logs can be inspected without direct database modification.
- [ ] Basic alerting/monitoring is configured for critical failures.
- [ ] API/Edge Function failures can be distinguished from device connectivity failures.
- [ ] Support/admin access is restricted and auditable where possible.
- [ ] There is a documented process for emergency credential revocation.

## 12. Privacy and Data Handling

- [ ] Production stores only required customer/device/location data.
- [ ] Retention requirements for historical location data are defined.
- [ ] Account/device deletion behaviour is defined.
- [ ] Production data is not routinely copied into DEV.
- [ ] Any production data used for troubleshooting in DEV is anonymised where appropriate.
- [ ] Logs do not unnecessarily retain precise location or authentication data.

## 13. Final Go-Live Gate

Before onboarding real customers, record:

- [ ] Production Supabase project ref.
- [ ] Production Vercel deployment/domain.
- [ ] Migration version/commit used for launch.
- [ ] Security Advisor review completed.
- [ ] End-to-end test completed successfully.
- [ ] Test data removed if it should not remain.
- [ ] Production test account/device clearly labelled if retained.
- [ ] DEV and PROD environment matrix updated.
- [ ] Rollback/recovery approach understood.
- [ ] Approval to accept real customer data recorded.

## Known Pre-Production Review Items

At the time this checklist was created, the current DEV environment had items that should be explicitly reviewed before PROD launch, including:

- anonymous/authenticated execution exposure of the `bluepaws_get_search_party_snapshot` `SECURITY DEFINER` function
- leaked-password protection configuration
- RLS-with-no-policy findings on sensitive/internal tables
- other `SECURITY DEFINER` functions used by ingestion and device command handling

These findings do not mean the design is necessarily incorrect. They mean each behaviour must be intentional and verified before production cutover.
