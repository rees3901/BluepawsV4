-- Read-only Family snapshot. Invoker RLS applies independently to every table.
-- No raw TLV, credentials, reset-reason interpretation, or command claiming.
create or replace function public.bluepaws_collar_feedback(requested_household_id uuid)
returns table(device_id integer, observation_id bigint, flags integer,
  rx_window_remaining_ms integer, command jsonb)
language sql stable security invoker set search_path = ''
as $$
  select d.device_id, o.id, o.flags::integer,
    case when p.heard_at is null or o.recorded_at > statement_timestamp() then 0
    else greatest(0, least(10000, floor(extract(epoch from
      (p.heard_at + interval '10 seconds' - statement_timestamp())) * 1000)))::integer end,
    case when c.id is null then null else jsonb_build_object(
      'id', c.id, 'device_id', c.device_id, 'command_type', c.command_type,
      'command_payload', c.command_payload, 'status', c.status,
      'requested_at', c.requested_at, 'expires_at', c.expires_at
    ) end
  from public.devices d
  left join lateral (
    select obs.id, obs.flags, obs.recorded_at, obs.effective_seen_at
    from public.observations obs
    where obs.device_guid16 = d.device_id and obs.household_id = d.household_id
    order by obs.effective_seen_at desc, obs.id desc limit 1
  ) o on true
  left join lateral (
    -- Earliest successful path; duplicate deliveries never restart the window.
    -- Conservatively require original hub time on LoRa. Missing/future clocks
    -- cannot manufacture a new receive opportunity from the cloud upload time.
    select least(o.recorded_at, o.effective_seen_at, path.first_received_at,
      case when path.ingest_path = 'lora_hub'
        then pg_catalog.to_timestamp(path.gateway_rx_time_unix::double precision)
        else o.recorded_at end) as heard_at
    from public.observation_paths path
    where path.observation_id = o.id and not path.offline_replay
      and (path.ingest_path = 'cellular_direct' or path.gateway_rx_time_unix > 0)
    order by path.first_received_at, path.id limit 1
  ) p on true
  left join lateral (
    select dc.* from public.device_commands dc
    where dc.household_id = d.household_id and dc.device_id = d.device_id
      and dc.requested_at > statement_timestamp() - interval '15 minutes'
    order by dc.requested_at desc, dc.id desc limit 1
  ) c on true
  where d.household_id = requested_household_id;
$$;
revoke all on function public.bluepaws_collar_feedback(uuid) from public, anon;
grant execute on function public.bluepaws_collar_feedback(uuid) to authenticated;

-- Same private, access-versioned Family topic as telemetry. Only status
-- transitions/creation notify; retries do not create a stream of UI events.
create or replace function private.broadcast_collar_command_feedback()
returns trigger language plpgsql security definer set search_path = '' as $$
declare v_access_version integer;
begin
  if tg_op = 'UPDATE' and new.status is not distinct from old.status then return null; end if;
  select h.access_version into strict v_access_version from public.households h where h.id = new.household_id;
  perform realtime.broadcast_changes(
    'household:' || new.household_id::text || ':v' || v_access_version::text,
    'COMMAND_CHANGED', tg_op, tg_table_name, tg_table_schema, new, old);
  return null;
end;
$$;
revoke all on function private.broadcast_collar_command_feedback() from public, anon, authenticated, service_role;
create trigger device_commands_broadcast_feedback after insert or update of status
on public.device_commands for each row execute function private.broadcast_collar_command_feedback();
