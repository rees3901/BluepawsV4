-- The public SECURITY INVOKER wrapper needs permission to call the guarded
-- private implementation. Preserve the existing definer boundary and keep all
-- table writes, claim/ACK helpers and anonymous execution restricted.
set lock_timeout = '5s';
set statement_timeout = '120s';

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

-- Do not grant EXECUTE until the NULL-safe Family membership guard is installed.
revoke all on function private.bluepaws_queue_device_command(integer, text, jsonb, interval) from public, anon;
revoke all on function public.bluepaws_queue_device_command(integer, text, jsonb, interval) from public, anon;
grant execute on function private.bluepaws_queue_device_command(integer, text, jsonb, interval) to authenticated;
grant execute on function public.bluepaws_queue_device_command(integer, text, jsonb, interval) to authenticated;

comment on function private.bluepaws_queue_device_command(integer, text, jsonb, interval) is
  'Guarded command writer called by the public invoker RPC. Requires auth.uid and owner/member membership of the target Family; privileged commands remain owner-only.';
