-- Run as a database administrator. All fixture mutations are rolled back;
-- no command is committed or transmitted to a real collar.
begin;
do $$
declare actor uuid; family uuid; collar integer; queued record; snapshot jsonb; denied boolean := false;
begin
  select m.user_id,d.household_id,d.device_id into actor,family,collar
  from public.devices d join public.household_members m
    on m.household_id=d.household_id and m.role='owner'
  where d.enabled and d.device_id between 3001 and 3004 limit 1;
  if actor is null then raise exception 'No fixture available'; end if;
  perform set_config('request.jwt.claim.sub',actor::text,true);
  select * into queued from public.bluepaws_queue_device_command(
    collar,'set_profile','{"profile":"power_save"}'::jsonb,interval '1 hour');
  if queued.expires_at < now()+interval '59 minutes' then raise exception 'Wrong TTL'; end if;
  update public.device_commands
  set requested_at=now()-interval '31 minutes',available_after=now()-interval '31 minutes',expires_at=now()+interval '29 minutes'
  where id=queued.id;
  select command into snapshot from public.bluepaws_collar_feedback(family) where device_id=collar;
  if snapshot->>'id' is distinct from queued.id::text then raise exception 'Long-running feedback missing'; end if;
  if not public.bluepaws_cancel_profile_command(queued.id) then raise exception 'Pending cancel failed'; end if;
  if public.bluepaws_cancel_profile_command(queued.id) then raise exception 'Double cancel incorrectly succeeded'; end if;
  select command into snapshot from public.bluepaws_collar_feedback(family) where device_id=collar;
  if snapshot->>'status' is distinct from 'cancelled' or snapshot->>'status_at' is null
    then raise exception 'Cancellation feedback missing'; end if;
  select * into queued from public.bluepaws_queue_device_command(
    collar,'set_profile','{"profile":"normal"}'::jsonb,interval '1 hour');
  update public.device_commands set status='sent',sent_at=now() where id=queued.id;
  if public.bluepaws_cancel_profile_command(queued.id) then raise exception 'Delivered command was cancelled'; end if;
  perform set_config('request.jwt.claim.sub',gen_random_uuid()::text,true);
  begin
    perform public.bluepaws_cancel_profile_command(queued.id);
  exception when insufficient_privilege then denied:=true;
  end;
  if not denied then raise exception 'Non-member cancellation allowed'; end if;
end $$;
rollback;
select 'PASS: hour TTL, 31-minute feedback, cancel, duplicate cancel, sent refusal, non-member rejection; all writes rolled back' as test_result;
