-- Additive LED command; existing Family authorization is retained.
set lock_timeout = '5s';
set statement_timeout = '120s';
alter table public.device_commands drop constraint device_commands_type_check;
alter table public.device_commands add constraint device_commands_type_check check
 (command_type in ('set_profile','request_status','force_report','enter_lost_alert','exit_lost_alert','reboot','debug_cadence','led_find'));
-- Preserve the installed validator and all its existing behaviour.
alter function private.bluepaws_validate_command_payload(text,jsonb)
 rename to bluepaws_validate_legacy_command_payload;
create function private.bluepaws_validate_command_payload(command_type text, command_payload jsonb)
returns void language plpgsql stable set search_path = '' as $$
begin
 if command_type <> 'led_find' then
  perform private.bluepaws_validate_legacy_command_payload(command_type, command_payload);
  return;
 end if;
 if jsonb_typeof(command_payload) is distinct from 'object'
   or command_payload ->> 'action' is null
   or command_payload ->> 'action' not in ('flash','repeat','stop')
   or (command_payload - 'action' - 'duration_s' - 'interval_s') <> '{}'::jsonb then
  raise exception using errcode='22023', message='Invalid LED Find payload';
 end if;
 if command_payload ->> 'action' = 'repeat' then
  if jsonb_typeof(command_payload->'duration_s') is distinct from 'number'
    or (command_payload->>'duration_s') !~ '^[0-9]+$'
    or (command_payload->>'duration_s')::numeric not between 60 and 3600
    or command_payload->'interval_s' is distinct from '60'::jsonb then
   raise exception using errcode='22023', message='Repeat requires 60..3600 seconds and a 60-second interval';
  end if;
 elsif command_payload <> jsonb_build_object('action',command_payload->>'action') then
  raise exception using errcode='22023', message='Flash/stop only accept action';
 end if;
end;
$$;
revoke all on function private.bluepaws_validate_command_payload(text,jsonb) from public, anon, authenticated;
create or replace function private.bluepaws_queue_device_command(
  requested_device_id integer,
  requested_command_type text,
  requested_payload jsonb default '{}'::jsonb,
  requested_expires_in interval default interval '1 hour'
)
returns table (
  id uuid,
  device_id integer,
  command_sequence_id integer,
  command_type text,
  command_payload jsonb,
  status text,
  expires_at timestamptz
)
language plpgsql
security definer
set search_path = ''
as $$
declare
  caller_id uuid := auth.uid();
  target_household_id uuid;
  caller_role text;
  normalized_type text := lower(btrim(requested_command_type));
  normalized_payload jsonb := coalesce(requested_payload, '{}'::jsonb);
  sequence_id integer;
begin
  if caller_id is null then
    raise exception using errcode = '42501', message = 'Authentication required';
  end if;

  select device.household_id
  into target_household_id
  from public.devices as device
  where device.device_id = requested_device_id
    and device.enabled = true;

  if target_household_id is null then
    raise exception using errcode = 'P0002', message = 'Device not found';
  end if;

  select member.role
  into caller_role
  from public.household_members as member
  where member.household_id = target_household_id
    and member.user_id = caller_id;

  -- SELECT INTO returns NULL when membership does not exist: fail closed.
  if caller_role is null or caller_role not in ('owner', 'member') then
    raise exception using errcode = '42501', message = 'Family membership required';
  end if;

  if normalized_type in ('reboot', 'debug_cadence') and caller_role <> 'owner' then
    raise exception using errcode = '42501', message = 'Owner role required for this command';
  end if;

  if normalized_type = 'led_find' and requested_device_id not between 3001 and 3004 then
    raise exception using errcode='22023', message='LED Find currently requires fitted personal collars 3001-3004';
  end if;

  perform private.bluepaws_validate_command_payload(normalized_type, normalized_payload);

  if normalized_type = 'set_profile'
    and normalized_payload ->> 'profile' = 'debug'
    and caller_role <> 'owner'
  then
    raise exception using errcode = '42501', message = 'Owner role required for Debug profile';
  end if;

  if requested_expires_in is null
    or requested_expires_in < interval '1 minute'
    or requested_expires_in > interval '24 hours'
  then
    raise exception using errcode = '22023', message = 'Command expiry must be between 1 minute and 24 hours';
  end if;

  if normalized_type in ('set_profile', 'enter_lost_alert', 'exit_lost_alert') then
    update public.device_commands as existing
    set
      status = 'cancelled',
      cancelled_at = now(),
      last_error = 'superseded_by_new_profile_command'
    where existing.device_id = requested_device_id
      and existing.status in ('pending', 'sent')
      and existing.command_type in ('set_profile', 'enter_lost_alert', 'exit_lost_alert');
  end if;

  if normalized_type = 'led_find' then
    update public.device_commands as existing
    set status = 'cancelled', cancelled_at = now(), last_error = 'superseded_by_new_led_find_command'
    where existing.device_id = requested_device_id and existing.command_type = 'led_find'
      and existing.status in ('pending', 'sent');
  end if;

  sequence_id := private.bluepaws_next_command_sequence(requested_device_id);

  return query
  insert into public.device_commands (
    household_id,
    device_id,
    command_sequence_id,
    command_type,
    command_payload,
    requested_by,
    expires_at
  )
  values (
    target_household_id,
    requested_device_id,
    sequence_id,
    normalized_type,
    normalized_payload,
    caller_id,
    now() + requested_expires_in
  )
  returning
    device_commands.id,
    device_commands.device_id,
    device_commands.command_sequence_id,
    device_commands.command_type,
    device_commands.command_payload,
    device_commands.status,
    device_commands.expires_at;
end;
$$;
