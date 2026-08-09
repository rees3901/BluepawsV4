-- Run only after an authenticated preview has proven household RLS and private
-- Realtime delivery. This intentionally removes the legacy anonymous surface
-- and activates bounded seven-day history retention.

set lock_timeout = '5s';
set statement_timeout = '120s';

drop policy if exists "Temporary public read" on public.positions;
revoke all on table public.positions from anon;
revoke all on table public.latest_positions from anon, authenticated;
drop view if exists public.latest_positions;

create extension if not exists pg_cron with schema pg_catalog;

create or replace function private.delete_expired_positions(batch_size integer default 50000)
returns integer
language plpgsql
security definer
set search_path = ''
as $$
declare
  deleted_rows integer;
begin
  delete from public.positions
  where id in (
    select id
    from public.positions
    where recorded_at < now() - interval '7 days'
    order by recorded_at
    limit greatest(1, least(batch_size, 50000))
  );
  get diagnostics deleted_rows = row_count;
  return deleted_rows;
end;
$$;

revoke execute on function private.delete_expired_positions(integer)
  from public, anon, authenticated;

do $$
declare
  existing_job_id bigint;
begin
  select jobid into existing_job_id
  from cron.job
  where jobname = 'bluepaws-delete-expired-positions';

  if existing_job_id is not null then
    perform cron.unschedule(existing_job_id);
  end if;

  perform cron.schedule(
    'bluepaws-delete-expired-positions',
    '* * * * *',
    'select private.delete_expired_positions(50000)'
  );
end
$$;

comment on function private.delete_expired_positions(integer) is
  'Deletes bounded batches of position history older than seven days.';
