import type { TelemetryDevice } from "@/types/telemetry";

export interface PositionRow {
  position_id: number;
  device_uid: number;
  household_id: string;
  message_id: number;
  latitude: number;
  longitude: number;
  battery: number | null;
  source: string;
  recorded_at: string;
  received_at: string;
  schema_version: number;
}

export function positionToTelemetryDevice(row: PositionRow): TelemetryDevice {
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

export function isPositionRow(value: unknown): value is PositionRow {
  if (!value || typeof value !== "object") return false;
  const row = value as Record<string, unknown>;
  return (
    typeof row.position_id === "number" &&
    typeof row.device_uid === "number" &&
    typeof row.household_id === "string" &&
    typeof row.message_id === "number" &&
    typeof row.latitude === "number" &&
    typeof row.longitude === "number" &&
    (row.battery === null || typeof row.battery === "number") &&
    typeof row.source === "string" &&
    typeof row.recorded_at === "string" &&
    typeof row.received_at === "string" &&
    typeof row.schema_version === "number" &&
    Number.isFinite(Date.parse(row.recorded_at))
  );
}

function clamp(value: number, minimum: number, maximum: number) {
  return Math.min(maximum, Math.max(minimum, value));
}
