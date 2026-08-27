-- Preserve the time at which an offline Home Hub originally heard a collar.
-- Cloud received_at remains immutable upload provenance; effective_seen_at is
-- the only timestamp used to determine presence freshness after replay.

set lock_timeout = '5s';
set statement_timeout = '120s';

alter table public.observations
  add column if not exists effective_seen_at timestamptz;

update public.observations
set effective_seen_at = received_at
where effective_seen_at is null;

alter table public.observations
  alter column effective_seen_at set default now(),
  alter column effective_seen_at set not null;

create index if not exists observations_device_effective_seen_idx
  on public.observations (device_guid16, effective_seen_at desc, id desc);

alter table public.observation_paths
  add column if not exists offline_replay boolean not null default false,
  add column if not exists gateway_local_id bigint;

alter table public.observation_paths
  drop constraint if exists observation_paths_gateway_local_id_positive;
alter table public.observation_paths
  add constraint observation_paths_gateway_local_id_positive
  check (gateway_local_id is null or gateway_local_id > 0);

create index if not exists observation_paths_gateway_replay_idx
  on public.observation_paths (gateway_guid16, gateway_local_id)
  where offline_replay and gateway_local_id is not null;

create or replace function private.update_device_presence_from_observation()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  update public.devices as device
  set
    last_seen_at = greatest(
      coalesce(device.last_seen_at, '-infinity'::timestamptz),
      new.effective_seen_at
    ),
    last_seen_status_code = new.status,
    last_seen_power_profile_code = new.power_profile,
    last_seen_tx_reason = new.tx_reason,
    last_seen_battery_mv = new.batt_mv
  where device.device_id = new.device_guid16
    and (
      device.last_seen_at is null
      or device.last_seen_at <= new.effective_seen_at
    );
  return new;
end;
$$;

-- Batch replay calls the existing, heavily-tested ingestion function inside
-- one database transaction, then corrects presence provenance before commit.
-- It deliberately performs no pending-command claim.
create or replace function public.ingest_tlv_observation_replay(
  p_protocol_version smallint,
  p_device_guid16 integer,
  p_msg_seq_id integer,
  p_time_unix bigint,
  p_status smallint,
  p_power_profile smallint,
  p_flags smallint,
  p_tx_reason smallint,
  p_gnss_valid boolean,
  p_latitude double precision,
  p_longitude double precision,
  p_batt_mv integer,
  p_acc_m integer,
  p_fix_age_s integer,
  p_sat_count integer,
  p_tlv_data jsonb,
  p_payload_hash text,
  p_payload_b64 text,
  p_hmac_body_b64 text,
  p_hmac_tag_hex text,
  p_ingest_path text,
  p_link_type text,
  p_gateway_guid16 integer,
  p_gateway_rx_time_unix bigint,
  p_link_rssi_dbm double precision,
  p_link_snr_db double precision,
  p_cell_rsrp_dbm double precision,
  p_cell_rsrq_db double precision,
  p_cell_sinr_db double precision,
  p_effective_seen_at timestamptz,
  p_gateway_local_id bigint
)
returns table (
  accepted boolean,
  duplicate boolean,
  observation_id bigint,
  position_id bigint,
  received_at timestamptz,
  error_code text
)
language plpgsql
security definer
set search_path = ''
as $$
declare
  v_result record;
  v_latest record;
begin
  if p_effective_seen_at is null
     or p_effective_seen_at > clock_timestamp() + interval '5 minutes'
     or p_gateway_local_id is null
     or p_gateway_local_id <= 0 then
    return query select false, false, null::bigint, null::bigint,
      clock_timestamp(), 'invalid_replay_provenance'::text;
    return;
  end if;

  select * into v_result
  from public.ingest_tlv_observation(
    p_protocol_version, p_device_guid16, p_msg_seq_id, p_time_unix,
    p_status, p_power_profile, p_flags, p_tx_reason, p_gnss_valid,
    p_latitude, p_longitude, p_batt_mv, p_acc_m, p_fix_age_s,
    p_sat_count, p_tlv_data, p_payload_hash, p_payload_b64,
    p_hmac_body_b64, p_hmac_tag_hex, p_ingest_path, p_link_type,
    p_gateway_guid16, p_gateway_rx_time_unix, p_link_rssi_dbm,
    p_link_snr_db, p_cell_rsrp_dbm, p_cell_rsrq_db, p_cell_sinr_db
  );

  if v_result.accepted and v_result.observation_id is not null then
    update public.observations as observation
    set effective_seen_at = least(observation.effective_seen_at, p_effective_seen_at)
    where observation.id = v_result.observation_id;

    update public.observation_paths as path
    set offline_replay = true,
        gateway_local_id = p_gateway_local_id
    where path.observation_id = v_result.observation_id
      and path.gateway_guid16 = p_gateway_guid16;

    select observation.* into v_latest
    from public.observations as observation
    where observation.device_guid16 = p_device_guid16
    order by observation.effective_seen_at desc, observation.id desc
    limit 1;

    if v_latest.id is not null then
      update public.devices
      set last_seen_at = v_latest.effective_seen_at,
          last_seen_status_code = v_latest.status,
          last_seen_power_profile_code = v_latest.power_profile,
          last_seen_tx_reason = v_latest.tx_reason,
          last_seen_battery_mv = v_latest.batt_mv
      where device_id = p_device_guid16;
    end if;
  end if;

  return query select
    v_result.accepted::boolean,
    v_result.duplicate::boolean,
    v_result.observation_id::bigint,
    v_result.position_id::bigint,
    v_result.received_at::timestamptz,
    v_result.error_code::text;
end;
$$;

revoke all on function public.ingest_tlv_observation_replay(
  smallint, integer, integer, bigint, smallint, smallint, smallint,
  smallint, boolean, double precision, double precision, integer,
  integer, integer, integer, jsonb, text, text, text, text, text,
  text, integer, bigint, double precision, double precision,
  double precision, double precision, double precision, timestamptz, bigint
) from public, anon, authenticated;

grant execute on function public.ingest_tlv_observation_replay(
  smallint, integer, integer, bigint, smallint, smallint, smallint,
  smallint, boolean, double precision, double precision, integer,
  integer, integer, integer, jsonb, text, text, text, text, text,
  text, integer, bigint, double precision, double precision,
  double precision, double precision, double precision, timestamptz, bigint
) to service_role;

comment on column public.observations.effective_seen_at is
  'Original presence time. For offline replay this is the Home Hub reception time; received_at remains cloud upload time.';
comment on column public.observation_paths.offline_replay is
  'True when this route receipt was uploaded by the dedicated offline batch replay endpoint.';
