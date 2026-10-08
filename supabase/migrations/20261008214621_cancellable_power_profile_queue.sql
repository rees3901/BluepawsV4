-- Only commands still in the cloud queue can be reliably recalled. The
-- conditional UPDATE serializes against command claiming on the same row.
create or replace function private.bluepaws_cancel_profile_command(requested_command_id uuid)
returns boolean language plpgsql security definer set search_path = '' as $$
declare caller_id uuid := auth.uid(); target_id uuid; changed_id uuid;
begin
  if caller_id is null then
    raise exception using errcode = '42501', message = 'Authentication required';
  end if;
  select command.id into target_id
  from public.device_commands command
  join public.household_members member on member.household_id = command.household_id
    and member.user_id = caller_id and member.role in ('owner', 'member')
  where command.id = requested_command_id and command.command_type = 'set_profile'
    and (command.command_payload ->> 'profile' <> 'debug' or member.role = 'owner');
  if target_id is null then
    raise exception using errcode = '42501', message = 'Profile command not accessible';
  end if;
  update public.device_commands command
  set status = 'cancelled', cancelled_at = now(), last_error = 'cancelled_by_user'
  where command.id = target_id and command.status = 'pending' and command.expires_at > now()
  returning command.id into changed_id;
  return changed_id is not null;
end;
$$;
create or replace function public.bluepaws_cancel_profile_command(requested_command_id uuid)
returns boolean language sql security invoker set search_path = '' as $$
  select private.bluepaws_cancel_profile_command(requested_command_id);
$$;
revoke all on function private.bluepaws_cancel_profile_command(uuid) from public, anon;
revoke all on function public.bluepaws_cancel_profile_command(uuid) from public, anon;
grant execute on function private.bluepaws_cancel_profile_command(uuid) to authenticated;
grant execute on function public.bluepaws_cancel_profile_command(uuid) to authenticated;

-- Preserve receive-window logic; expose pending feedback for its actual TTL,
-- and terminal feedback for fifteen minutes after its terminal event.
create or replace function public.bluepaws_collar_feedback(requested_household_id uuid)
returns table(device_id integer, observation_id bigint, flags integer, rx_window_remaining_ms integer, command jsonb)
language sql stable security invoker set search_path = '' as $$
  select d.device_id, o.id, o.flags::integer,
    case when p.heard_at is null or p.heard_at > statement_timestamp() then 0
    else greatest(0, least(10000, floor(extract(epoch from
      (p.heard_at + interval '10 seconds' - statement_timestamp())) * 1000)))::integer end,
    case when c.id is null then null else jsonb_build_object(
      'id', c.id, 'device_id', c.device_id, 'command_type', c.command_type,
      'command_payload', c.command_payload, 'status', c.status,
      'requested_at', c.requested_at, 'expires_at', c.expires_at,
      'status_at', coalesce(c.acknowledged_at, c.cancelled_at, c.sent_at, c.requested_at)
    ) end
  from public.devices d
  left join lateral (
    select obs.id, obs.flags, obs.recorded_at, obs.effective_seen_at
    from public.observations obs
    where obs.device_guid16 = d.device_id and obs.household_id = d.household_id
    order by obs.effective_seen_at desc, obs.id desc limit 1
  ) o on true
  left join lateral (
    select least(path.first_received_at,
      case when path.ingest_path = 'lora_hub'
        then pg_catalog.to_timestamp(path.gateway_rx_time_unix::double precision)
        else path.first_received_at end) as heard_at
    from public.observation_paths path
    where path.observation_id = o.id and not path.offline_replay
      and (path.ingest_path = 'cellular_direct' or path.gateway_rx_time_unix > 0)
    order by path.first_received_at, path.id limit 1
  ) p on true
  left join lateral (
    select dc.* from public.device_commands dc
    where dc.household_id = d.household_id and dc.device_id = d.device_id
      and greatest(dc.requested_at, dc.expires_at, dc.acknowledged_at, dc.cancelled_at) > statement_timestamp() - interval '15 minutes'
    order by dc.requested_at desc, dc.id desc limit 1
  ) c on true
  where d.household_id = requested_household_id;
$$;
revoke all on function public.bluepaws_collar_feedback(uuid) from public, anon;
grant execute on function public.bluepaws_collar_feedback(uuid) to authenticated;
