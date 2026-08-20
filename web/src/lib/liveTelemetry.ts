import { createClient } from "@/lib/supabase/server";
import { isPositionRow, positionToTelemetryDevice } from "@/lib/telemetryRows";
import type { TelemetryDevice } from "@/types/telemetry";

export interface LiveTelemetrySnapshot {
  devices: TelemetryDevice[];
  householdId: string | null;
  accessVersion: number | null;
  error: string | null;
}

export async function getLiveTelemetrySnapshot(householdId: string): Promise<LiveTelemetrySnapshot> {
  try {
    const supabase = await createClient();
    const householdResult = await supabase
      .from("households")
      .select("access_version")
      .eq("id", householdId)
      .single();

    if (householdResult.error) throw householdResult.error;
    if (!Number.isInteger(householdResult.data.access_version)) throw new Error("Family access version is unavailable");

    const accessVersion = householdResult.data.access_version;
    const positionResult = await supabase
      .from("device_latest_positions")
      .select("position_id,device_uid,household_id,message_id,latitude,longitude,battery,battery_mv,status_code,power_profile_code,flags,tx_reason,ingest_path,link_type,link_rssi_dbm,link_snr_db,source,recorded_at,received_at,schema_version")
      .eq("household_id", householdId);

    if (positionResult.error) {
      console.error("Unable to load latest device positions from Supabase", positionResult.error);
      return {
        devices: [],
        householdId,
        accessVersion,
        error: "Unable to read the latest pet positions from Supabase.",
      };
    }

    return {
      devices: (positionResult.data ?? []).filter(isPositionRow).map(positionToTelemetryDevice),
      householdId,
      accessVersion,
      error: null,
    };
  } catch (error) {
    console.error("Unable to load live telemetry from Supabase", error);
    return {
      devices: [],
      householdId: null,
      accessVersion: null,
      error: "Unable to read live telemetry from Supabase.",
    };
  }
}
