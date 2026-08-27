-- The gateway-authenticated snapshot reads history through the server-only role.
-- Existing ingestion writes through its restricted RPC; no write grants needed.
-- Do not grant additional privileges to anon or authenticated.
grant select on public.observations, public.observation_paths to service_role;
