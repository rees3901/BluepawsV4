-- TLV protocol v1.2 addressing migration.
--
-- The on-wire field formerly named device_guid16 is now source_id16 and keeps
-- exactly the same provisioned numeric identity. destination_id16 is new at
-- bytes 3..4 of a v1.2 packet. Existing database device_id/device_uid naming is
-- intentionally preserved to avoid a high-risk cross-application rename.

set lock_timeout = '5s';
set statement_timeout = '120s';

-- v1.1 packets use protocol_version 1. v1.2 changes the header layout and uses
-- on-wire protocol version 2 so a decoder cannot silently confuse the offsets.
alter table public.observations
  drop constraint if exists observations_protocol_version;

alter table public.observations
  add constraint observations_protocol_version
  check (protocol_version in (1, 2));

-- Explicit storage alias for the new protocol terminology. This is deliberately
-- generated from device_guid16 because source_id16 is not a second identity.
alter table public.observations
  add column if not exists source_id16 integer
  generated always as (device_guid16) stored;

-- Decode the v1.2 logical destination directly from the immutable raw packet.
-- payload_b64 contains the complete application packet. Offsets 3 and 4 are
-- destination_id16 little-endian in v1.2. v1.1 rows have no destination field.
alter table public.observations
  add column if not exists destination_id16 integer
  generated always as (
    case
      when protocol_version = 2 then
        get_byte(decode(payload_b64, 'base64'), 3)
        + (get_byte(decode(payload_b64, 'base64'), 4) * 256)
      else null
    end
  ) stored;

alter table public.observations
  drop constraint if exists observations_source_id16_range;
alter table public.observations
  add constraint observations_source_id16_range
  check (source_id16 between 1 and 65534);

alter table public.observations
  drop constraint if exists observations_destination_id16_range;
alter table public.observations
  add constraint observations_destination_id16_range
  check (destination_id16 is null or destination_id16 between 0 and 65535);

create index if not exists observations_source_destination_recorded_idx
  on public.observations (source_id16, destination_id16, recorded_at desc);

comment on column public.observations.source_id16 is
  'TLV v1.2 protocol name for the same provisioned identity stored historically as device_guid16.';
comment on column public.observations.destination_id16 is
  'TLV v1.2 logical destination. 0x0000 = cloud/backend, 0xFFFF = broadcast.';

-- Provisioning rules for new records. NOT VALID means existing historical IDs
-- are not scanned/rejected during this migration, but new/updated rows must obey
-- the v1.2 allocation policy. A later cleanup can VALIDATE the constraints.
alter table public.gateways
  drop constraint if exists gateways_v12_hub_id_role;
alter table public.gateways
  add constraint gateways_v12_hub_id_role
  check (
    gateway_guid16 between 1 and 65534
    and mod(gateway_guid16, 16) = 0
  ) not valid;

alter table public.devices
  drop constraint if exists devices_v12_collar_id_role;
alter table public.devices
  add constraint devices_v12_collar_id_role
  check (
    device_id between 1 and 65534
    and mod(device_id, 16) <> 0
  ) not valid;

comment on constraint gateways_v12_hub_id_role on public.gateways is
  'v1.2: provisioned hub IDs are non-zero/non-broadcast multiples of 16.';
comment on constraint devices_v12_collar_id_role on public.devices is
  'v1.2: collar IDs are non-zero/non-broadcast IDs that are not multiples of 16.';
