-- Read-only assertions; can run with db query --linked --file (no Docker).
do $$
begin
  assert has_table_privilege('service_role', 'public.observations', 'SELECT'),
    'Snapshot service cannot read observations';
  assert has_table_privilege('service_role', 'public.observation_paths', 'SELECT'),
    'Snapshot service cannot read observation paths';
  assert not has_table_privilege('anon', 'public.observations', 'SELECT'),
    'Anonymous users must not read raw history';
  assert not has_table_privilege('anon', 'public.observation_paths', 'SELECT'),
    'Anonymous users must not read raw transport metadata';
end
$$;
