-- Qualify device_commands columns that share names with RETURNS TABLE output
-- columns. PostgreSQL otherwise treats those references as ambiguous inside
-- PL/pgSQL and the command queue/claim functions fail at runtime.

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

  if caller_role not in ('owner', 'member') then
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

  if requested_expires_in < interval '1 minute' or requested_expires_in > interval '24 hours' then
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
create or replace function public.bluepaws_claim_next_device_command(
  requested_device_id integer,
  requested_transport text
)
returns table (
  id uuid,
  command_sequence_id integer,
  command_type text,
  command_payload jsonb,
  expires_at timestamptz
)
language plpgsql
security definer
set search_path = ''
as $$
begin
  if requested_transport not in ('cellular_direct', 'lora_hub') then
    raise exception using errcode = '22023', message = 'Unsupported command transport';
  end if;

  update public.device_commands as existing
  set
    status = 'expired',
    last_error = 'expired_before_delivery'
  where existing.device_id = requested_device_id
    and existing.status in ('pending', 'sent')
    and existing.expires_at <= now();

  return query
  update public.device_commands as command
  set
    status = 'sent',
    sent_at = coalesce(command.sent_at, now()),
    attempts = command.attempts + 1
  where command.id = (
    select candidate.id
    from public.device_commands as candidate
    where candidate.device_id = requested_device_id
      and candidate.status in ('pending', 'sent')
      and candidate.available_after <= now()
      and candidate.expires_at > now()
    order by candidate.requested_at, candidate.id
    for update skip locked
    limit 1
  )
  returning
    command.id,
    command.command_sequence_id,
    command.command_type,
    command.command_payload,
    command.expires_at;
end;
$$;
