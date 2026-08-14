import { createClient } from "@/lib/supabase/server";
import { isPositionRow, positionToTelemetryDevice } from "@/lib/telemetryRows";
import type { TelemetryDevice } from "@/types/telemetry";

export interface LiveTelemetrySnapshot {
  devices: TelemetryDevice[];
  householdId: string | null;
  error: string | null;
}

export async function getLiveTelemetrySnapshot(): Promise<LiveTelemetrySnapshot> {
  try {
    const supabase = await createClient();
    const membership = await supabase
      .from("household_members")
      .select("household_id")
      .order("joined_at", { ascending: true })
      .limit(1)
      .maybeSingle();

    if (membership.error) throw membership.error;
    if (!membership.data) {
      return { devices: [], householdId: null, error: null };
    }

    const { data, error } = await supabase
      .from("device_latest_positions")
      .select("position_id,device_uid,household_id,message_id,latitude,longitude,battery,battery_mv,status_code,power_profile_code,flags,tx_reason,ingest_path,link_type,link_rssi_dbm,link_snr_db,source,recorded_at,received_at,schema_version")
      .eq("household_id", membership.data.household_id);

    if (error) throw error;

    return {
      devices: (data ?? []).filter(isPositionRow).map(positionToTelemetryDevice),
      householdId: membership.data.household_id,
      error: null,
    };
  } catch (error) {
    console.error("Unable to load live telemetry from Supabase", error);
    return {
      devices: [],
      householdId: null,
      error: "Unable to read live telemetry from Supabase.",
    };
  }
}
