import type { SupabaseClient } from "@supabase/supabase-js";
import type { PositionRow } from "./telemetryRows";
import { GPS_FULL_REPORT_REASONS } from "./gpsReporting.ts";

// Quality comes only from the displayed position. Newer full no-fix reports
// contribute a separate failure count, never a replacement GPS timestamp.
export async function withGnssQuality(client: SupabaseClient, householdId: string, positions: PositionRow[]): Promise<PositionRow[]> {
  const ids = [...new Set(positions.flatMap(row => typeof row.observation_id === "number" ? [row.observation_id] : []))];
  if (!ids.length) return positions;
  try {
    const qualityRead = client.from("observations")
      .select("id,device_guid16,gnss_valid,acc_m,sat_count,fix_age_s,recorded_at")
      .eq("household_id", householdId).in("id", ids);
    const failuresRead = Promise.all(positions.map(async row => {
      if (!Number.isInteger(row.observation_id)) return undefined;
      try {
        const since = new Date(Math.min(Date.parse(row.recorded_at), Date.parse(row.received_at))).toISOString();
        // Bounded, household/device-scoped, backed by the existing
        // (household_id, device_guid16, recorded_at) index. Observations are
        // deduplicated by packet identity, so LoRa/LTE retries count once.
        const { data, error } = await client.from("observations")
          .select("id").eq("household_id", householdId).eq("device_guid16", row.device_uid)
          .in("tx_reason", [...GPS_FULL_REPORT_REASONS]).eq("gnss_valid", false)
          .gt("recorded_at", since).gt("id", row.observation_id!)
          .order("recorded_at", { ascending: false }).limit(3)
          .abortSignal(AbortSignal.timeout(3000));
        return error ? undefined : data?.length ?? 0;
      } catch {
        return undefined;
      }
    }));
    const [{ data, error }, failures] = await Promise.all([qualityRead, failuresRead]);
    if (error) return positions;
    return positions.map((position, index) => {
      const row = failures[index] === undefined ? position : { ...position, gpsFailedReports: failures[index] };
      const observation = data?.find(item => item.id === row.observation_id && item.device_guid16 === row.device_uid);
      if (!observation?.gnss_valid || !Number.isFinite(observation.acc_m) || !Number.isFinite(observation.sat_count) || !Number.isFinite(observation.fix_age_s) || !Number.isFinite(Date.parse(observation.recorded_at))) return row;
      return { ...row, gnss: { accuracyM: observation.acc_m, satellites: observation.sat_count, fixAgeS: observation.fix_age_s, recordedAt: observation.recorded_at } };
    });
  } catch {
    // Quality is supplementary: don't hide working positions when its read fails.
    return positions;
  }
}
