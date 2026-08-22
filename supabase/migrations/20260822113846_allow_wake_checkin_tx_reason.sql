-- Promote TX reason 7 to WAKE_CHECKIN across TLV ingestion storage.
-- Existing rows remain valid; this only widens the accepted enum range.

alter table public.observations
  drop constraint if exists observations_tx_reason_range,
  add constraint observations_tx_reason_range check (tx_reason between 0 and 7);

alter table public.positions
  drop constraint if exists positions_tx_reason_range,
  add constraint positions_tx_reason_range check (tx_reason is null or tx_reason between 0 and 7);

alter table public.device_latest_positions
  drop constraint if exists latest_positions_tx_reason_range,
  add constraint latest_positions_tx_reason_range check (tx_reason is null or tx_reason between 0 and 7);

comment on column public.observations.tx_reason is
  'TLV v1.1 TX reason enum. Values 0..7 are assigned; 7 is WAKE_CHECKIN.';

comment on column public.positions.tx_reason is
  'TLV v1.1 TX reason enum projected from accepted TLV observations; 7 is WAKE_CHECKIN.';

comment on column public.device_latest_positions.tx_reason is
  'TLV v1.1 TX reason enum projected from latest position telemetry; 7 is WAKE_CHECKIN.';

alter table public.devices
  add column if not exists last_seen_at timestamptz,
  add column if not exists last_seen_status_code smallint,
  add column if not exists last_seen_power_profile_code smallint,
  add column if not exists last_seen_tx_reason smallint,
  add column if not exists last_seen_battery_mv integer;

comment on column public.devices.last_seen_at is
  'Most recent accepted collar observation time, including no-GNSS WAKE_CHECKIN presence packets.';
comment on column public.devices.last_seen_tx_reason is
  'TX reason from the most recent accepted observation; 7 is WAKE_CHECKIN.';

create or replace function private.update_device_presence_from_observation()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
begin
  update public.devices as device
  set
    last_seen_at = greatest(coalesce(device.last_seen_at, '-infinity'::timestamptz), new.received_at),
    last_seen_status_code = new.status,
    last_seen_power_profile_code = new.power_profile,
    last_seen_tx_reason = new.tx_reason,
    last_seen_battery_mv = new.batt_mv
  where device.device_id = new.device_guid16
    and (
      device.last_seen_at is null
      or device.last_seen_at <= new.received_at
    );

  return new;
end;
$$;

drop trigger if exists observations_update_device_presence on public.observations;
create trigger observations_update_device_presence
  after insert on public.observations
  for each row
  execute function private.update_device_presence_from_observation();
