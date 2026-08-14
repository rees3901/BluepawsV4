-- Let Family Owners edit only the human-readable Family name. Household UUIDs
-- and every other tenancy/access field remain unavailable to client updates.

set lock_timeout = '5s';
set statement_timeout = '120s';

drop policy if exists "Owners update Family names" on public.households;
create policy "Owners update Family names"
  on public.households for update to authenticated
  using (private.current_user_is_household_owner(id))
  with check (private.current_user_is_household_owner(id));

revoke update on table public.households from authenticated;
grant update (name) on table public.households to authenticated;

comment on policy "Owners update Family names" on public.households is
  'Allows a current Family Owner to edit only the Family display name; column privileges protect all identifiers and access fields.';
