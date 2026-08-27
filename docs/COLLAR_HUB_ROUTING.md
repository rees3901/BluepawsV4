# Affiliated Home Hub routing — 27 August 2026

## Behaviour

The collar testbed is source **1001 / 03E9**, affiliated with Home Hub
**16 / 0010**. Root `platformio.ini` provisions both identities explicitly:

```ini
-DMY_DEVICE_ID=1001
-DMY_HOME_HUB_ID=0x0010
```

For another collar/hub pairing, provision the correct IDs at build time. The
affiliation remains in firmware flash across reboot; this change does not alter
the persisted runtime-state format or infer pairing from nearby radio traffic.
Other build environments must supply `MY_HOME_HUB_ID` explicitly (build flag or
local ignored `collar_secrets.h`). Missing, zero, broadcast or non-hub IDs fail
compilation. This is bench provisioning, not a new end-user pairing flow.

- Telemetry, BOOT, WAKE_CHECKIN, forced reports and timeout alerts target that hub.
- ACK/status/find replies target the command's sender, preserving correlation.
- The hub already filters for its own destination (plus legacy cloud/broadcast
  receive compatibility). A packet addressed to a different hub is ignored.
- Home and Portable relay when connected; Off-Grid journals locally and replays
  later. No hub mode, AP, BLE, authentication or replay policy is changed here.
- LTE fallback uses exactly the same signed bytes, including destination 0010.
  The backend authenticates the collar bearer, confirms an enabled destination
  hub in its Family, then performs the existing HMAC and deduplication checks.
- The hub still uses its **gateway** bearer, not the collar's bearer.

See [the canonical v1.2 contract](TLV_PROTOCOL_V1_2.md), section 8.

## Independent timestamp bug

The reported packet's `time_unix=1769444419` is in January 2026. The former
compiler-date parser searched for an entire `Mmm dd yyyy` string in a list of
three-letter month names, so it always fell back to January. That can leave
newly uploaded observations older than the current map position. Destination
0000 alone was not proof of the missing update: the hub already accepted it.

The collar PlatformIO pre-build hook now embeds a UTC epoch from the build
computer's clock instead of parsing local-time compiler macros. This also
avoids a BST/DST offset. Rebuild shortly before bench testing, with the PC clock
correct. Build time plus uptime is only an approximate testbed/fallback clock:
it does not solve time recovery after a much later reboot or replace the future
GNSS/modem clock. No-GNSS presence semantics and last-valid-position retention
remain unchanged.

## Rollout and bench verification

No database migration, Vercel deployment or credential rotation is required.

1. Deploy the updated `ingest-position` function before testing hub-addressed
   **LTE** fallback; old deployments reject a nonzero LTE destination:

   ```powershell
   npx --yes supabase@latest functions deploy ingest-position --project-ref ykcdaonkvwemedotdpdr
   ```

2. Build and upload the collar from the repository root, with COM23 free:

   ```powershell
   py -m platformio run -e collar -t upload --upload-port COM23
   ```

   Use the installed PlatformIO Python environment instead if `py` does not
   have PlatformIO. Root `platformio.ini` includes the UTC build hook.
   No hub upload is needed solely for this routing change.

3. Confirm collar boot says `Affiliated Home Hub: 0010 (16)`. Force a fresh
   report and observe COM11/COM7: source 03E9, destination 0010, current timestamp.
   In the raw header the first five bytes are `02 E9 03 10 00`.
4. Online: verify the hub receives the frame and logs cloud HTTP 201 (or 200
   duplicate). Check `position_updated` and report timestamps, not just HTTP
   success. A no-GNSS wake check-in updates presence, not position.
5. Off-Grid: verify the same address is locally received and journalled without
   immediate POST; reconnect to replay without rewriting the collar packet.
6. On an LTE-capable test path send the **same bytes** directly, using the collar
   bearer and `cellular_direct` wrapper without gateway fields. It should be
   idempotent with the hub copy. A different-Family/disabled destination must fail.

Do not claim the live radio/cloud pipeline is verified until these checks run.

## Offline regression checks

```powershell
node tools/test_collar_hub_routing.mjs
node --test supabase/functions/ingest-position/tlv.test.ts supabase/functions/ingest-position/routing.test.ts
py -3.11 -m unittest discover -s tools -p test_firmware_build_time.py
py -m platformio run -e collar -e hub
```

The native test compiles all seven actual collar packet-initializer call sites,
checks explicit uplink/reply addresses and HMAC coverage, and exercises the hub's
receive filter and Home/Portable/Off-Grid relay predicate. The handler tests use
synthetic credentials and an in-memory database stub (including a stand-in for
the existing SQL HMAC/dedup checks); they do not contact Supabase.
