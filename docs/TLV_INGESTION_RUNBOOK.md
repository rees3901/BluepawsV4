# TLV ingestion deployment and provisioning runbook

This runbook introduces authenticated Bluepaws v1.1 TLV telemetry without
removing the existing JSON test contract. Apply it to staging first. Do not put
real bearer tokens, HMAC keys, Supabase server keys, or Vault values in Git,
screenshots, logs, chat, or browser code.

## 1. Verify locally

From the repository root:

```powershell
Set-Location web
npm test
npm run lint
npm run typecheck
npm run build
Set-Location ..
python -m unittest discover -s tools -p "test_*.py"
npx --yes deno check supabase/functions/ingest-position/index.ts
npx --yes deno test supabase/functions/ingest-position/tlv.test.ts
```

## 2. Apply the database migration

Confirm that the CLI is linked to the intended staging project, inspect the
plan, and then push:

```powershell
npx --yes supabase@latest migration list --linked
npx --yes supabase@latest db push --linked --dry-run
npx --yes supabase@latest db push --linked
```

The migration adds:

- `device_hmac_keys`, whose rows reference encrypted Supabase Vault secrets;
- `gateways` and `gateway_ingest_credentials` for LoRa relays;
- immutable `observations` and aggregated `observation_paths`;
- TLV projection columns on `positions` and `device_latest_positions`;
- a service-role-only atomic ingestion RPC.

Verify the migration in the SQL Editor:

```sql
select
  to_regclass('public.observations') is not null as observations_exists,
  to_regclass('public.observation_paths') is not null as paths_exists,
  to_regclass('public.device_hmac_keys') is not null as keys_exist,
  to_regprocedure(
    'public.ingest_tlv_observation(smallint,integer,integer,bigint,smallint,smallint,smallint,smallint,boolean,double precision,double precision,integer,integer,integer,integer,jsonb,text,text,text,text,text,text,integer,bigint,double precision,double precision,double precision,double precision,double precision)'
  ) is not null as rpc_exists;
```

All four values must be `true`.

## 3. Provision a collar

The LTE wrapper uses the device's existing bearer token. The binary packet
also needs its own random 32-byte HMAC key. Generate the HMAC key locally:

```bash
python3 -c "import base64,secrets; print(base64.b64encode(secrets.token_bytes(32)).decode())"
```

Keep that one-time Base64 value in the collar/simulator secret store. In the
Supabase SQL Editor, create its Vault secret and map the returned UUID to the
device. Replace placeholders locally before running:

```sql
select vault.create_secret(
  '<32-byte-hmac-key-as-base64>',
  'bluepaws-device-1001-hmac-v1',
  'Bluepaws TLV HMAC key for device 1001, version 1'
) as vault_secret_id;

insert into public.device_hmac_keys (
  device_id,
  key_version,
  vault_secret_id
)
values (1001, 1, '<vault-secret-id-returned-above>');
```

Vault stores the key encrypted at rest. `device_hmac_keys` stores only its Vault
UUID. The Edge Function does not return the key or read it into browser code.

For rotation, insert the next key version first. Once deployed collars have
switched, set `valid_until` on the previous row and eventually set `enabled` to
`false`. The RPC accepts any enabled key inside its validity window.

## 4. Optionally provision a LoRa gateway

Generate a gateway bearer token and its SHA-256 hash locally:

```bash
python3 -c "import hashlib,secrets; t=secrets.token_urlsafe(32); print('TOKEN='+t); print('SHA256='+hashlib.sha256(t.encode()).hexdigest())"
```

Keep `TOKEN` only on the gateway. Insert only `SHA256` into Supabase:

```sql
insert into public.gateways (
  gateway_guid16,
  household_id,
  display_name
)
values (
  22,
  '<same-household-uuid-as-the-test-collars>',
  'Bluepaws Test Hub'
);

insert into public.gateway_ingest_credentials (
  gateway_guid16,
  token_hash
)
values (22, '<sha256-from-the-local-command>');
```

The JSON wrapper represents gateway `22` as the four-character hexadecimal
string `"0016"`. A gateway can relay only collars in its own household.

## 5. Deploy the Edge Function

The function performs its own bearer authentication, so deploy with platform
JWT verification disabled:

```powershell
npx --yes supabase@latest functions deploy ingest-position --no-verify-jwt
```

The hosted runtime provides `SUPABASE_URL` and `SUPABASE_SERVICE_ROLE_KEY`.
Never add the service-role key to the VPS simulator.

## 6. Configure the VPS simulator

In the VPS's private `tools/vps_devices.json`, keep collar and gateway identities
in separate arrays inside one credentials bundle:

```json
{
  "schema_version": 1,
  "devices": [
    {
      "device_id": 1001,
      "bearer_token": "device-bearer-token-issued-during-provisioning",
      "hmac_key_b64": "the-same-base64-key-stored-in-vault"
    }
  ],
  "gateways": [
    {
      "gateway_guid16": "0016",
      "display_name": "Bluepaws Test Hub",
      "bearer_token": "gateway-plaintext-token"
    }
  ]
}
```

Protect the file and send a short LTE test:

```bash
chmod 600 tools/vps_devices.json
python3 tools/tlv_telemetry_simulator.py --device-count 1 --iterations 5 --interval 2
```

To test the same immutable collar packet through the single gateway in the
bundle:

```bash
python3 tools/tlv_telemetry_simulator.py --transport lora_hub --device-count 1 --iterations 5 --interval 2
```

With multiple bundled gateways, add `--gateway-guid16 0016`. Explicit gateway
environment variables remain supported as temporary overrides.

New observations return HTTP `201`. Exact packet retries return `200` with
`"duplicate": true`; route receipts are aggregated rather than duplicating the
map point. Invalid outer bearer or inner HMAC authentication returns `401`.

## 7. Verify data isolation and projection

After the simulator runs, inspect only non-secret operational data:

```sql
select
  o.id,
  o.device_guid16,
  o.msg_seq_id,
  o.recorded_at,
  o.gnss_valid,
  o.payload_hash,
  p.ingest_path,
  p.route_key,
  p.receipt_count,
  p.last_received_at
from public.observations as o
join public.observation_paths as p on p.observation_id = o.id
order by o.received_at desc
limit 25;

select
  device_uid,
  recorded_at,
  battery_mv,
  status_code,
  power_profile_code,
  link_type
from public.device_latest_positions
order by recorded_at desc;
```

Then sign in as members of two different staging households. Confirm each user
can read only their household's observations and Realtime map updates. Also
test an invalid tag, an exact retry, two delivery paths, an out-of-order packet,
and a packet with GNSS validity cleared. Invalid GNSS must be retained as an
observation but must not move the pet marker.

After applying the migration, run Supabase's security and performance advisors
and resolve any findings before production rollout.
