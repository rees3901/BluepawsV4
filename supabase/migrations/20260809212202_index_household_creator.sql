-- Cover the auth.users foreign key used when ownership records are removed.

create index households_created_by_idx on public.households (created_by);

;
