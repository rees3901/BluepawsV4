-- Add the first production-shaped collar downlink command queue.
-- Commands are routed by device_id, not by IP address. LTE-direct collars can
-- receive one pending command in the ingest response; Home Hubs can poll/relay
-- commands during collar RX windows.

set lock_timeout = '5s';
set statement_timeout = '120s';

create table public.device_commands (
  id uuid primary key default gen_random_uuid(),
  household_id uuid not null references public.households(id) on delete cascade,
  device_id integer not null references public.devices(device_id) on delete cascade,
  command_sequence_id integer not null,
  command_type text not null,
  command_payload jsonb not null default '{}'::jsonb,
  status text not null default 'pending',
  requested_by uuid references auth.users(id) on delete set null,
  requested_at timestamptz not null default now(),
  available_after timestamptz not null default now(),
  expires_at timestamptz not null default now() + interval '15 minutes',
  sent_at timestamptz,
  acknowledged_at timestamptz,
  cancelled_at timestamptz,
  attempts integer not null default 0,
  last_error text,
  constraint device_commands_device_range check (device_id between 1 and 65535),
  constraint device_commands_sequence_range check (command_sequence_id between 1 and 65535),
  constraint device_commands_type_check check (
    command_type in (
      'set_profile',
      'request_status',
      'force_report',
      'enter_lost_alert',
      'exit_lost_alert',
      'reboot',
      'debug_cadence'
    )
  ),
  constraint device_commands_payload_object check (jsonb_typeof(command_payload) = 'object'),
  constraint device_commands_status_check check (
    status in ('pending', 'sent', 'acked', 'expired', 'cancelled', 'failed')
  ),
  constraint device_commands_time_order check (
    expires_at > requested_at
    and available_after >= requested_at
    and (sent_at is null or sent_at >= requested_at)
    and (acknowledged_at is null or acknowledged_at >= requested_at)
    and (cancelled_at is null or cancelled_at >= requested_at)
  ),
  constraint device_commands_attempts_range check (attempts between 0 and 100),
  constraint device_commands_last_error_length check (last_error is null or length(last_error) <= 200)
);

create index device_commands_device_sequence_idx
  on public.device_commands (device_id, command_sequence_id, requested_at desc);

create index device_commands_pending_delivery_idx
  on public.device_commands (device_id, available_after, requested_at, id)
  where status in ('pending', 'sent');

create index device_commands_household_device_requested_idx
  on public.device_commands (household_id, device_id, requested_at desc);

alter table public.device_commands enable row level security;

create policy "Family members read device commands"
  on public.device_commands for select to authenticated
  using (
    exists (
      select 1
      from public.household_members as member
      where member.household_id = device_commands.household_id
        and member.user_id = (select auth.uid())
    )
  );

revoke all on table public.device_commands from anon, authenticated, service_role;
grant select on table public.device_commands to authenticated;
grant select, insert, update on table public.device_commands to service_role;

create or replace function private.bluepaws_validate_command_payload(
  command_type text,
  command_payload jsonb
)
returns void
language plpgsql
stable
set search_path = ''
as $$
declare
  profile text;
  fallback_profile text;
  interval_s integer;
begin
  if jsonb_typeof(command_payload) <> 'object' then
    raise exception using errcode = '22023', message = 'Command payload must be a JSON object';
  end if;

  if command_type = 'set_profile' then
    profile := command_payload ->> 'profile';
    if profile not in ('normal', 'power_save', 'active', 'lost_alert') then
      raise exception using errcode = '22023', message = 'set_profile requires a valid profile';
    end if;
  elsif command_type = 'request_status' then
    if command_payload <> '{}'::jsonb then
      raise exception using errcode = '22023', message = 'request_status payload must be empty';
    end if;
  elsif command_type = 'force_report' then
    if command_payload ? 'gnss' and jsonb_typeof(command_payload -> 'gnss') <> 'boolean' then
      raise exception using errcode = '22023', message = 'force_report.gnss must be boolean when supplied';
    end if;
  elsif command_type = 'enter_lost_alert' then
    if command_payload <> '{}'::jsonb then
      raise exception using errcode = '22023', message = 'enter_lost_alert payload must be empty';
    end if;
  elsif command_type = 'exit_lost_alert' then
    fallback_profile := coalesce(command_payload ->> 'fallback_profile', 'active');
    if fallback_profile not in ('normal', 'power_save', 'active') then
      raise exception using errcode = '22023', message = 'exit_lost_alert fallback_profile must be non-emergency';
    end if;
  elsif command_type = 'reboot' then
    if coalesce(command_payload ->> 'reason', 'owner_request') not in ('owner_request', 'support') then
      raise exception using errcode = '22023', message = 'reboot reason must be owner_request or support';
    end if;
  elsif command_type = 'debug_cadence' then
    if jsonb_typeof(command_payload -> 'enabled') <> 'boolean' then
      raise exception using errcode = '22023', message = 'debug_cadence.enabled is required';
    end if;
    interval_s := nullif(command_payload ->> 'interval_s', '')::integer;
    if interval_s is null or interval_s not between 5 and 3600 then
      raise exception using errcode = '22023', message = 'debug_cadence.interval_s must be 5..3600';
    end if;
  else
    raise exception using errcode = '22023', message = 'Unsupported command type';
  end if;
end;
$$;

create or replace function private.bluepaws_next_command_sequence(target_device_id integer)
returns integer
language plpgsql
volatile
set search_path = ''
as $$
declare
  next_sequence integer;
begin
  select coalesce(max(command_sequence_id), 0) + 1
  into next_sequence
  from public.device_commands
  where device_id = target_device_id;

  if next_sequence > 65535 then
    next_sequence := 1;
  end if;

  while exists (
    select 1
    from public.device_commands
    where device_id = target_device_id
      and command_sequence_id = next_sequence
      and requested_at > now() - interval '7 days'
  ) loop
    next_sequence := next_sequence + 1;
    if next_sequence > 65535 then
      next_sequence := 1;
    end if;
  end loop;

  return next_sequence;
end;
$$;

create or replace function private.bluepaws_queue_device_command(
  requested_device_id integer,
  requested_command_type text,
  requested_payload jsonb default '{}'::jsonb,
  requested_expires_in interval default interval '15 minutes'
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

  if requested_expires_in < interval '1 minute' or requested_expires_in > interval '24 hours' then
    raise exception using errcode = '22023', message = 'Command expiry must be between 1 minute and 24 hours';
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

create or replace function public.bluepaws_queue_device_command(
  requested_device_id integer,
  requested_command_type text,
  requested_payload jsonb default '{}'::jsonb,
  requested_expires_in interval default interval '15 minutes'
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
language sql
security invoker
set search_path = ''
as $$
  select *
  from private.bluepaws_queue_device_command(
    requested_device_id,
    requested_command_type,
    requested_payload,
    requested_expires_in
  );
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

  update public.device_commands
  set
    status = 'expired',
    last_error = 'expired_before_delivery'
  where device_id = requested_device_id
    and status in ('pending', 'sent')
    and expires_at <= now();

  return query
  update public.device_commands as command
  set
    status = 'sent',
    sent_at = now(),
    attempts = command.attempts + 1
  where command.id = (
    select candidate.id
    from public.device_commands as candidate
    where candidate.device_id = requested_device_id
      and candidate.status in ('pending', 'sent')
      and candidate.available_after <= now()
      and candidate.expires_at > now()
      and candidate.attempts < 5
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

create or replace function public.bluepaws_ack_device_command(
  requested_device_id integer,
  acked_command_sequence_id integer
)
returns table (
  id uuid,
  command_sequence_id integer,
  command_type text,
  status text,
  acknowledged_at timestamptz
)
language plpgsql
security definer
set search_path = ''
as $$
begin
  if acked_command_sequence_id not between 1 and 65535 then
    raise exception using errcode = '22023', message = 'ACK sequence must be 1..65535';
  end if;

  return query
  update public.device_commands as command
  set
    status = 'acked',
    acknowledged_at = now()
  where command.device_id = requested_device_id
    and command.command_sequence_id = acked_command_sequence_id
    and command.status in ('pending', 'sent')
  returning
    command.id,
    command.command_sequence_id,
    command.command_type,
    command.status,
    command.acknowledged_at;
end;
$$;

revoke execute on function private.bluepaws_validate_command_payload(text, jsonb) from public, anon, authenticated;
revoke execute on function private.bluepaws_next_command_sequence(integer) from public, anon, authenticated;
revoke all on function private.bluepaws_queue_device_command(integer, text, jsonb, interval) from public, anon, authenticated;
revoke all on function public.bluepaws_queue_device_command(integer, text, jsonb, interval) from public, anon, authenticated;
revoke all on function public.bluepaws_claim_next_device_command(integer, text) from public, anon, authenticated;
revoke all on function public.bluepaws_ack_device_command(integer, integer) from public, anon, authenticated;

grant execute on function public.bluepaws_queue_device_command(integer, text, jsonb, interval) to authenticated;
grant execute on function public.bluepaws_claim_next_device_command(integer, text) to service_role;
grant execute on function public.bluepaws_ack_device_command(integer, integer) to service_role;

comment on table public.device_commands is
  'Queued collar downlink commands. Commands are routed by device_id and delivered only when a collar checks in.';
comment on column public.device_commands.command_sequence_id is
  'Compact 16-bit sequence used by the collar in TLV_ACKED_MSG_SEQ_ID; the UUID id remains the backend audit identity.';
comment on function public.bluepaws_queue_device_command(integer, text, jsonb, interval) is
  'Queues a collar command for an authenticated Family member, with owner-only restrictions for debug/reboot commands.';
comment on function public.bluepaws_claim_next_device_command(integer, text) is
  'Service-role delivery helper used by Edge Functions/Home Hub paths to atomically claim the next pending command.';
comment on function public.bluepaws_ack_device_command(integer, integer) is
  'Service-role ACK helper used when a collar reports TLV_ACKED_MSG_SEQ_ID.';
