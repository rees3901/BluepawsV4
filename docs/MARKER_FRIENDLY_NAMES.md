# Marker friendly names

The avatar editor includes **Pet name** for collars and **Hub name** for Home Hubs.
Save changes persists the name alongside the selected emoji/photo and marker colour.
The display name is used on cards, map popups and related controls. Expanded cards
retain Device ID (or Hub ID) for reference.

## Storage and permissions

- Collar names reuse `devices.display_name`; appearance data remains in
  `device_appearances`. The security-invoker `bluepaws_save_device_marker` RPC
  writes both atomically under existing Family-scoped RLS. Owners and members
  can edit; anonymous visitors, search-party guests and other Families cannot.
- Hub names reuse `hub_presence.display_name` and are saved in the same update
  as hub appearance. Existing hub Family permissions and photo storage apply.
- IDs, Family assignment, bearer tokens, HMACs and transmitted TLV packets never
  change when a marker is renamed. Names are not sent to collars as commands.
- Renaming broadcasts metadata to Family dashboards without changing last-seen.
  New telemetry and page reloads retain the name. Search-party snapshots expose
  the same friendly name while preserving token, expiry and Family checks.
- Collar names are trimmed, nonempty, single-line and at most 80 Unicode characters.
  Hub names also retain the existing 64-byte UTF-8 limit used by hub storage.

The Home Hub snapshot already reads collar display names, so future snapshot
refreshes bring cloud names into its cache. The existing Off-Grid name editor
remains a **hub-local override**, not a cloud rename. Its expanded cards now also
show Device ID. No automatic cloud/local ownership or naming transfer is added.

## Deployment

Apply `20260828084628_add_marker_friendly_names.sql` before deploying the web UI:

```powershell
npx --yes supabase@latest db push --linked --skip-vault --dry-run
npx --yes supabase@latest db push --linked --skip-vault
```

Run from the canonical `BluepawsV4-git` repository. Inspect the dry-run list first;
do not apply unrelated pending migrations unintentionally. Then merge the PR so
Vercel builds the updated UI. No Edge Function or collar firmware deployment is
needed. The local expanded Device ID row requires a normal hub web-assets update;
off-grid naming itself already exists.

## Verification

```powershell
npm --prefix web run typecheck
npm --prefix web run lint
npm --prefix web test
npm --prefix web run test:feedback
node tools/test_marker_names_db.mjs
py -3.11 -m unittest tools/test_hub_public_assets.py
```

The isolated SQL test uses the same PGlite dependency as the existing feedback
database tests (`.pio/feedback-tests/node_modules/@electric-sql/pglite`). If absent:

```powershell
npm install --prefix .pio/feedback-tests --no-save @electric-sql/pglite
```

Browser smoke check: open an avatar editor, rename Device 1001 to Mittens, save,
and reopen it. Confirm the card and map popup use Mittens, expanded details still
show 1001, cancelling an edit changes nothing, and a blank name cannot be saved.
Check a subsequent collar report and page refresh preserve Mittens without a rename
itself lighting the awake indicator or resetting the last-seen timer.
