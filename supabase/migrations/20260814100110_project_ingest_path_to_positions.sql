-- Project the authenticated transport wrapper path into position history and
-- the maintained current-position table. The existing latest-position trigger
-- then includes the path in private household Realtime broadcasts.

set lock_timeout = '5s';
set statement_timeout = '120s';

alter table public.positions
  add column if not exists ingest_path text;

alter table public.device_latest_positions
  add column if not exists ingest_path text;

do $$
begin
  if not exists (
    select 1
    from pg_constraint
    where conname = 'positions_ingest_path_check'
      and conrelid = 'public.positions'::regclass
  ) then
    alter table public.positions
      add constraint positions_ingest_path_check
      check (ingest_path is null or ingest_path in ('lora_hub', 'cellular_direct'));
  end if;

  if not exists (
    select 1
    from pg_constraint
    where conname = 'latest_positions_ingest_path_check'
      and conrelid = 'public.device_latest_positions'::regclass
  ) then
    alter table public.device_latest_positions
      add constraint latest_positions_ingest_path_check
      check (ingest_path is null or ingest_path in ('lora_hub', 'cellular_direct'));
  end if;
end
$$;

-- A packet can be received through both routes. The position row represents
-- the route which first accepted the observation, so backfill deterministically
-- from the earliest stored observation path.
update public.positions as position
set ingest_path = (
  select path.ingest_path
  from public.observation_paths as path
  where path.observation_id = position.observation_id
  order by path.first_received_at, path.id
  limit 1
)
where position.ingest_path is null
  and position.observation_id is not null;

update public.device_latest_positions as latest
set ingest_path = position.ingest_path
from public.positions as position
where position.id = latest.position_id
  and latest.ingest_path is distinct from position.ingest_path;

create or replace function private.sync_latest_position()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  insert into public.device_latest_positions (
    device_uid,
    household_id,
    position_id,
    message_id,
    latitude,
    longitude,
    battery,
    source,
    recorded_at,
    received_at,
    schema_version,
    observation_id,
    battery_mv,
    status_code,
    power_profile_code,
    flags,
    tx_reason,
    ingest_path,
    link_type,
    link_rssi_dbm,
    link_snr_db
  )
  values (
    new.device_uid,
    new.household_id,
    new.id,
    new.message_id,
    new.latitude,
    new.longitude,
    new.battery,
    new.source,
    new.recorded_at,
    new.received_at,
    new.schema_version,
    new.observation_id,
    new.battery_mv,
    new.status_code,
    new.power_profile_code,
    new.flags,
    new.tx_reason,
    new.ingest_path,
    new.link_type,
    new.link_rssi_dbm,
    new.link_snr_db
  )
  on conflict (device_uid) do update
  set
    household_id = excluded.household_id,
    position_id = excluded.position_id,
    message_id = excluded.message_id,
    latitude = excluded.latitude,
    longitude = excluded.longitude,
    battery = excluded.battery,
    source = excluded.source,
    recorded_at = excluded.recorded_at,
    received_at = excluded.received_at,
    schema_version = excluded.schema_version,
    observation_id = excluded.observation_id,
    battery_mv = excluded.battery_mv,
    status_code = excluded.status_code,
    power_profile_code = excluded.power_profile_code,
    flags = excluded.flags,
    tx_reason = excluded.tx_reason,
    ingest_path = excluded.ingest_path,
    link_type = excluded.link_type,
    link_rssi_dbm = excluded.link_rssi_dbm,
    link_snr_db = excluded.link_snr_db
  where
    excluded.recorded_at > public.device_latest_positions.recorded_at
    or (
      excluded.recorded_at = public.device_latest_positions.recorded_at
      and excluded.message_id > public.device_latest_positions.message_id
    );

  return new;
end;
$$;

revoke execute on function private.sync_latest_position()
  from public, anon, authenticated;

-- Preserve the existing service-role-only ingestion RPC signature and patch
-- only its position projection. Assertions make schema drift fail closed.
do $migration$
declare
  function_signature constant regprocedure :=
    'public.ingest_tlv_observation(smallint,integer,integer,bigint,smallint,smallint,smallint,smallint,boolean,double precision,double precision,integer,integer,integer,integer,jsonb,text,text,text,text,text,text,integer,bigint,double precision,double precision,double precision,double precision,double precision)'::regprocedure;
  current_definition text;
  corrected_definition text;
  old_columns constant text := E'        tx_reason,\n        link_type,\n        link_rssi_dbm,';
  new_columns constant text := E'        tx_reason,\n        ingest_path,\n        link_type,\n        link_rssi_dbm,';
  old_values constant text := E'        p_tx_reason,\n        p_link_type,\n        p_link_rssi_dbm,';
  new_values constant text := E'        p_tx_reason,\n        p_ingest_path,\n        p_link_type,\n        p_link_rssi_dbm,';
  old_update constant text := E'        tx_reason = excluded.tx_reason,\n        link_type = excluded.link_type,\n        link_rssi_dbm = excluded.link_rssi_dbm,';
  new_update constant text := E'        tx_reason = excluded.tx_reason,\n        ingest_path = excluded.ingest_path,\n        link_type = excluded.link_type,\n        link_rssi_dbm = excluded.link_rssi_dbm,';
begin
  select pg_get_functiondef(function_signature)
  into current_definition;

  if strpos(current_definition, new_columns) > 0
    and strpos(current_definition, new_values) > 0
    and strpos(current_definition, new_update) > 0 then
    return;
  end if;

  if strpos(current_definition, old_columns) = 0
    or strpos(current_definition, old_values) = 0
    or strpos(current_definition, old_update) = 0 then
    raise exception 'ingest_tlv_observation does not contain the expected position projection';
  end if;

  corrected_definition := replace(current_definition, old_columns, new_columns);
  corrected_definition := replace(corrected_definition, old_values, new_values);
  corrected_definition := replace(corrected_definition, old_update, new_update);

  if corrected_definition = current_definition then
    raise exception 'ingest_tlv_observation position projection was not updated';
  end if;

  execute corrected_definition;
end
$migration$;

comment on column public.positions.ingest_path is
  'Authenticated HTTPS ingress path for the observation that created this position.';
comment on column public.device_latest_positions.ingest_path is
  'Ingress path projected into snapshots and private household Realtime broadcasts.';
