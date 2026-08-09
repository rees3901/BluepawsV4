import { createSupabaseClient } from "@/lib/supabase";
import type { TelemetryDevice } from "@/types/telemetry";

const POSITION_QUERY_LIMIT = 500;

interface PositionRow {
  id: number;
  device_uid: number;
  message_id: number;
  latitude: number;
  longitude: number;
  battery: number | null;
  source: string | null;
  recorded_at: string;
}

export interface LiveTelemetrySnapshot {
  devices: TelemetryDevice[];
  error: string | null;
}

export async function getLiveTelemetrySnapshot(): Promise<LiveTelemetrySnapshot> {
  try {
    const supabase = createSupabaseClient();
    const { data, error } = await supabase
      .from("positions")
      .select("id,device_uid,message_id,latitude,longitude,battery,source,recorded_at")
      .order("recorded_at", { ascending: false, nullsFirst: false })
      .limit(POSITION_QUERY_LIMIT);

    if (error) throw error;

    const latestByDevice = new Map<number, PositionRow>();
    for (const candidate of data ?? []) {
      if (!isPositionRow(candidate) || latestByDevice.has(candidate.device_uid)) continue;
      latestByDevice.set(candidate.device_uid, candidate);
    }

    return {
      devices: [...latestByDevice.values()].map(positionToTelemetryDevice),
      error: null,
    };
  } catch (error) {
    console.error("Unable to load live telemetry from Supabase", error);
    return {
      devices: [],
      error: "Unable to read live telemetry from Supabase.",
    };
  }
}

function positionToTelemetryDevice(row: PositionRow): TelemetryDevice {
  const recordedAt = Date.parse(row.recorded_at);
  return {
    id: row.device_uid,
    name: `Device ${row.device_uid}`,
    seq: row.message_id,
    time: Math.floor(recordedAt / 1000),
    status: "Out",
    profile: "Normal",
    error: "None",
    lat: row.latitude,
    lon: row.longitude,
    hasGps: true,
    batt: 0,
    batteryPercent: row.battery === null ? null : clamp(row.battery, 0, 100),
    rssi: null,
    snr: null,
    bleHome: false,
    cellular: false,
    lastUpdate: recordedAt,
    source: row.source,
  };
}

function isPositionRow(value: unknown): value is PositionRow {
  if (!value || typeof value !== "object") return false;
  const row = value as Record<string, unknown>;
  return (
    typeof row.id === "number" &&
    typeof row.device_uid === "number" &&
    typeof row.message_id === "number" &&
    typeof row.latitude === "number" &&
    typeof row.longitude === "number" &&
    (row.battery === null || typeof row.battery === "number") &&
    (row.source === null || typeof row.source === "string") &&
    typeof row.recorded_at === "string" &&
    Number.isFinite(Date.parse(row.recorded_at))
  );
}

function clamp(value: number, minimum: number, maximum: number) {
  return Math.min(maximum, Math.max(minimum, value));
}
