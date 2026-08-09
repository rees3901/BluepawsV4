-- Realtime Authorization exposes the requested channel through
-- realtime.topic(); do not depend on a synthetic realtime.messages row value.

drop policy if exists "Household members receive position broadcasts" on realtime.messages;
create policy "Household members receive position broadcasts"
  on realtime.messages for select to authenticated
  using (
    extension = 'broadcast'
    and exists (
      select 1 from public.household_members as member
      where member.user_id = (select auth.uid())
        and (select realtime.topic()) = 'household:' || member.household_id::text
    )
  );
