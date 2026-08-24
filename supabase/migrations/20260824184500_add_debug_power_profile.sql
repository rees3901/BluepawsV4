-- Add TLV v1.1 power_profile=4 for development-only Debug mode.
-- This keeps values 5..15 reserved while allowing the whole cloud pipeline to
-- accept, store, and display noisy bench-test packets.

set lock_timeout = '5s';
set statement_timeout = '120s';

alter table public.observations
  drop constraint if exists observations_profile_range,
  add constraint observations_profile_range
    check (power_profile between 0 and 4);

alter table public.positions
  drop constraint if exists positions_power_profile_range,
  add constraint positions_power_profile_range
    check (power_profile_code is null or power_profile_code between 0 and 4);

alter table public.latest_positions
  drop constraint if exists latest_positions_power_profile_range,
  add constraint latest_positions_power_profile_range
    check (power_profile_code is null or power_profile_code between 0 and 4);
