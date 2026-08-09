-- Lock the ingestion registry to server-side callers and remove privileges that
-- Supabase default grants may have applied when the tables/view were created.

revoke all on table public.devices from anon, authenticated, service_role;
revoke all on table public.device_ingest_credentials from anon, authenticated, service_role;
revoke all on table public.positions from service_role;
revoke all on table public.latest_positions from anon, authenticated, service_role;

grant select on table public.devices to service_role;
grant select on table public.device_ingest_credentials to service_role;
grant select, insert on table public.positions to service_role;
grant select on table public.latest_positions to anon, authenticated, service_role;

do $$
declare
  identity_sequence text;
begin
  identity_sequence := pg_get_serial_sequence('public.positions', 'id');
  if identity_sequence is not null then
    execute format('revoke all on sequence %s from service_role', identity_sequence);
    execute format('grant usage, select on sequence %s to service_role', identity_sequence);
  end if;
end
$$;

-- Explicit deny policies document the registry boundary and prevent advisor
-- warnings while the service role continues to bypass RLS server-side.
do $$
begin
  if not exists (
    select 1 from pg_policies
    where schemaname = 'public'
      and tablename = 'devices'
      and policyname = 'Registry is server-only'
  ) then
    create policy "Registry is server-only"
      on public.devices
      for select
      to anon, authenticated
      using (false);
  end if;

  if not exists (
    select 1 from pg_policies
    where schemaname = 'public'
      and tablename = 'device_ingest_credentials'
      and policyname = 'Credentials are server-only'
  ) then
    create policy "Credentials are server-only"
      on public.device_ingest_credentials
      for select
      to anon, authenticated
      using (false);
  end if;
end
$$;

-- This SECURITY DEFINER function is an internal event-trigger target, not a
-- client RPC. Revoking direct execution does not prevent its event trigger from
-- enabling RLS on newly-created public tables.
revoke execute on function public.rls_auto_enable() from public, anon, authenticated;
