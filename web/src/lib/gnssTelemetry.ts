import type { SupabaseClient } from "@supabase/supabase-js";
import type { PositionRow } from "./telemetryRows";

// Read only the observations backing the displayed positions, not newer no-fix check-ins.
export async function withGnssQuality(client: SupabaseClient, householdId: string, positions: PositionRow[]): Promise<PositionRow[]> {
  const ids = [...new Set(positions.flatMap(row => typeof row.observation_id === "number" ? [row.observation_id] : []))];
  if (!ids.length) return positions;
  try {
    const { data, error } = await client.from("observations")
      .select("id,device_guid16,gnss_valid,acc_m,sat_count,fix_age_s,recorded_at")
      .eq("household_id", householdId).in("id", ids);
    if (error) return positions;
    return positions.map(row => {
      const observation = data?.find(item => item.id === row.observation_id && item.device_guid16 === row.device_uid);
      if (!observation?.gnss_valid || !Number.isFinite(observation.acc_m) || !Number.isFinite(observation.sat_count) || !Number.isFinite(observation.fix_age_s) || !Number.isFinite(Date.parse(observation.recorded_at))) return row;
      return { ...row, gnss: { accuracyM: observation.acc_m, satellites: observation.sat_count, fixAgeS: observation.fix_age_s, recordedAt: observation.recorded_at } };
    });
  } catch {
    // Quality is supplementary: don't hide working positions when its read fails.
    return positions;
  }
}
