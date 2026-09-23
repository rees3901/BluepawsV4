# Documentation

Documentation is grouped by subject rather than by when it was written:

- `protocol/`: canonical TLV specifications and collar downlink commands.
- `firmware/collar/`: collar behaviour, hardware testbeds, and bench records.
- `firmware/home-hub/`: Home Hub architecture, behaviour, and testbed notes.
- `diagnostics/`: passive receivers and diagnostic firmware notes.
- `operations/`: deployment runbooks and production-readiness checks.
- `development/`: environment strategy, decisions, and the active backlog.
- `standards/`: agreed implementation standards that should be checked before changing behaviour.
- `branding/`: corporate colours, typography, logo rules and reusable design tokens.

Product-level setup instructions remain beside the implementation they describe.

## Canonical References

Use this table before creating a new document. If a topic already has a clear
owner, update that document instead of adding a parallel note.

| Question | Canonical document | Notes |
|---|---|---|
| What is the current telemetry packet format? | `protocol/TLV_PROTOCOL_V1_2.md` | Current on-wire TLV format, header fields, addressing, flags, TX reasons, auth tag and decoder procedure. |
| What changed from the older TLV format? | `protocol/TLV_PROTOCOL_V1_2.md` | `protocol/TLV_PROTOCOL_V1_1.md` remains useful history but v1.2 is the implementation target. |
| What does a wake check-in, stale fix or no-GNSS packet mean? | `protocol/TLV_PROTOCOL_V1_2.md` and `firmware/collar/COLLAR_RUNTIME_DECISIONS.md` | Do not create a separate telemetry semantics document unless these meanings are first moved out of the protocol/runtime docs. |
| How do collar commands route and expire? | `protocol/COLLAR_DOWNLINK_COMMANDS.md` | Covers LTE-direct, Home Hub delivery, command dictionary, expiry, ACKs and security notes. |
| What are the agreed collar power profiles? | `standards/POWER_PROFILES.md` | Human-readable standard. Verify compiled values in `../shared/lib/BluepawsProtocol/bp_config.h`. |
| How does the collar behave at runtime? | `firmware/collar/COLLAR_RUNTIME_DECISIONS.md` | Runtime flow, BLE home behaviour, boot reports, persistence, button behaviour and Lost Alert behaviour. |
| Which Walter/GM02SP vendor facts guide hardware, GNSS, LTE and power decisions? | `firmware/collar/WALTER_GM02SP_UPSTREAM_REFERENCE.md` | Source-linked QuickSpot guide; distinguishes Walter board facts from bare-modem assumptions and links the BluePaws testbed. |
| How does the Home Hub behave in Home, Portable or Off-Grid use? | `firmware/home-hub/HOME_HUB_RUNTIME_DECISIONS.md` and `firmware/home-hub/HOME_HUB_OFFGRID_ARCHITECTURE.md` | Runtime roles and off-grid/local behaviour. |
| How does the hub report itself to the cloud? | `firmware/home-hub/HUB_SELF_PRESENCE.md` | Hub self-presence, hub reporting profiles, contact overdue rules and collar receive indicator behaviour. |
| How is TLV ingestion deployed and tested? | `operations/TLV_INGESTION_RUNBOOK.md` | Local verification, migrations, provisioning, Edge Function deployment and simulator checks. |
| What must be true before production launch? | `operations/PRODUCTION_READINESS_CHECKLIST.md` | Launch gate checklist across environments, database, RLS, auth, device security, Edge Functions, storage and privacy. |
| How are dev and production separated? | `development/DEV_TO_PROD_STRATEGY.md` and `development/ENVIRONMENT_MATRIX.md` | Environment model, GitHub/Vercel/Supabase strategy, secrets, simulator safety and data movement rules. |
| What remains to build or decide? | `development/TODO.md` | Active backlog and open elaboration candidates. |
| What is the current visual/brand standard? | `branding/BLUE_PAWS_BRAND_GUIDE.md` | Colours, typography, logo usage, UI themes and CSS tokens. |
| How do we inspect live LoRa/TLV traffic? | `diagnostics/T190_RADIO_MONITOR.md` | Passive T190 sniffer build and usage notes. |

## Tidy-Up Rules

- Prefer updating the current canonical document over creating a new one.
- Create a new `standards/` document only when the topic cuts across multiple
  implementation areas and needs a single agreed reference.
- Keep test evidence and dated bench notes in `firmware/*` unless they become
  durable rules. Promote only the durable rule into `standards/`.
- Keep implementation setup instructions next to the implementation they describe,
  for example tool READMEs under `tools/` and app setup under `web/`.
- When a standard repeats values from code, name the code file that must be
  checked before implementation.
- Do not duplicate TLV field meanings outside `protocol/TLV_PROTOCOL_V1_2.md`
  unless the protocol document is also updated to point at the new owner.
