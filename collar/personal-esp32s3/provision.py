"""Prepare fresh personal credentials and fail-on-conflict registration SQL.

Does not connect to a database. Apply the private SQL only to the verified live
backend, then select a collar with --select. Never overwrite a credential bundle.
"""
import argparse
import base64
import hashlib
import json
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from generate_tlv_credentials import generate_device, generate_gateway, credential_bundle, private_write

HERE = Path(__file__).resolve().parent
PRIVATE = HERE / ".secrets"

def build_sql(bundle, household, register_gateway=True):
    household = str(uuid.UUID(household))
    ids = ",".join(str(d["device_id"]) for d in bundle["devices"])
    gateway = bundle["gateways"][0]
    hub = int(gateway["gateway_guid16"], 16)
    lines = ["begin;", "lock table public.devices, public.gateways in share row exclusive mode;",
             "do $$ begin",
             f"if exists(select 1 from public.devices where device_id in ({ids})) then raise exception 'Collar ID already exists; no credentials changed'; end if;",
             f"if exists(select 1 from public.gateways where gateway_guid16 = {hub}) then raise exception 'Hub ID already exists; no credentials changed'; end if;",
             f"if not exists(select 1 from public.household_members where household_id = '{household}'::uuid and role='owner') then raise exception 'Family has no active owner'; end if;",
             "end $$;"]
    for d in bundle["devices"]:
        ident = d["device_id"]
        digest = hashlib.sha256(d["bearer_token"].encode()).hexdigest()
        key = d["hmac_key_b64"]
        # Generated material is base64/hex; never interpolates user prose.
        lines += [f"insert into public.devices(device_id,household_id,display_name,enabled) values ({ident},'{household}','Collar {ident}',true);",
                  f"insert into public.device_ingest_credentials(device_id,token_hash,enabled,rotated_at) values ({ident},'{digest}',true,now());",
                  f"with s as (select vault.create_secret('{key}','bluepaws-device-{ident}-hmac-v1','Personal collar key') as id) insert into public.device_hmac_keys(device_id,key_version,vault_secret_id) select {ident},1,id from s;"]
    digest = hashlib.sha256(gateway["bearer_token"].encode()).hexdigest()
    lines += [f"insert into public.gateways(gateway_guid16,household_id,display_name,enabled) values ({hub},'{household}','Personal Heltec Home Hub',true);",
              f"insert into public.gateway_ingest_credentials(gateway_guid16,token_hash,enabled,rotated_at) values ({hub},'{digest}',true,now());",
              "commit;"]
    if not register_gateway:
        lines = [line for line in lines if "gateway_guid16" not in line]
    return "\n".join(lines) + "\n"

def collar_header(bundle, ident):
    device = next(d for d in bundle["devices"] if d["device_id"] == ident)
    key = base64.b64decode(device["hmac_key_b64"], validate=True)
    if len(key) != 32:
        raise ValueError("Invalid HMAC key length")
    octets = ",".join(f"0x{b:02x}" for b in key)
    return ("#pragma once\n#include <stdint.h>\n"
            f"constexpr uint16_t PERSONAL_DEVICE_ID = {ident};\n"
            f"constexpr uint16_t PERSONAL_HUB_ID = 0x{bundle['gateways'][0]['gateway_guid16']};\n"
            f"static const uint8_t PERSONAL_HMAC_KEY[32] = {{{octets}}};\n")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--household")
    parser.add_argument("--endpoint")
    parser.add_argument("--ssid", default="Reesnet Guest")
    parser.add_argument("--ids", nargs="+", type=int, default=[3001, 3002, 3003, 3004])
    parser.add_argument("--hub", default="0030")
    parser.add_argument("--select", type=int)
    args = parser.parse_args()
    path = PRIVATE / "credentials.json"
    if args.select is not None:
        bundle = json.loads(path.read_text())
        private_write(PRIVATE / "personal_config.h", collar_header(bundle, args.select), overwrite=True)
        print(f"Selected collar {args.select}; secret header written locally")
        return
    if not args.household or not args.endpoint:
        parser.error("--household and --endpoint are required for fresh provisioning")
    if not args.endpoint.startswith("https://") or not args.endpoint.endswith("/functions/v1/ingest-position"):
        parser.error("Use the verified live HTTPS ingest-position endpoint")
    if len(set(args.ids)) != len(args.ids):
        parser.error("Device IDs must be unique")
    devices = [generate_device(i) for i in args.ids]
    gateway = generate_gateway(args.hub, "Personal Heltec Home Hub")
    bundle = credential_bundle(devices, gateway)
    bundle.update(household_id=str(uuid.UUID(args.household)), endpoint=args.endpoint)
    private_write(path, json.dumps(bundle, indent=2), overwrite=False)
    private_write(PRIVATE / "register.sql", build_sql(bundle, args.household), overwrite=False)
    for ident in args.ids:
        private_write(PRIVATE / f"collar_{ident}.h", collar_header(bundle, ident), overwrite=False)
    hub_header = ("#pragma once\n"
        f"#define GATEWAY_GUID16 0x{gateway.gateway_guid16}\n"
        f"#define WIFI_STA_SSID {json.dumps(args.ssid)}\n#define WIFI_STA_PASS \"\"\n"
        f"#define CLOUD_ENDPOINT {json.dumps(args.endpoint)}\n"
        f"#define CLOUD_BEARER_TOKEN {json.dumps(gateway.bearer_token)}\n"
        "#define HUB_PROVISIONING_MODE_DEFAULT false\n")
    private_write(ROOT / "hub/personal-heltec/.secrets/hub_secrets.h", hub_header, overwrite=False)
    print(f"Prepared {len(devices)} distinct collar credentials and hub {gateway.gateway_guid16}; not yet registered")

if __name__ == "__main__":
    main()
