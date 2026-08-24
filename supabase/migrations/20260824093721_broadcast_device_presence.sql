create or replace function private.broadcast_device_presence()
returns trigger
language plpgsql
security definer
set search_path = ''
as $$
declare
  household_access_version integer;
begin
  select household.access_version
  into strict household_access_version
  from public.households as household
  where household.id = new.household_id;

  perform realtime.broadcast_changes(
    'household:' || new.household_id::text || ':v' || household_access_version::text,
    'DEVICE_PRESENCE',
    tg_op,
    tg_table_name,
    tg_table_schema,
    new,
    old
  );
  return null;
end;
$$;

revoke execute on function private.broadcast_device_presence()
  from public, anon, authenticated, service_role;

drop trigger if exists devices_broadcast_presence on public.devices;
create trigger devices_broadcast_presence
  after update of
    last_seen_at,
    last_seen_status_code,
    last_seen_power_profile_code,
    last_seen_tx_reason,
    last_seen_battery_mv
  on public.devices
  for each row
  when (
    old.last_seen_at is distinct from new.last_seen_at
    or old.last_seen_status_code is distinct from new.last_seen_status_code
    or old.last_seen_power_profile_code is distinct from new.last_seen_power_profile_code
    or old.last_seen_tx_reason is distinct from new.last_seen_tx_reason
    or old.last_seen_battery_mv is distinct from new.last_seen_battery_mv
  )
  execute function private.broadcast_device_presence();
