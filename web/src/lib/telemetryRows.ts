import type { TelemetryDevice } from "@/types/telemetry";

export interface PositionRow {
  position_id: number;
  device_uid: number;
  household_id: string;
  message_id: number;
  latitude: number;
  longitude: number;
  battery: number | null;
  battery_mv: number | null;
  status_code: number | null;
  power_profile_code: number | null;
  flags: number | null;
  tx_reason: number | null;
  link_type: "lora" | "lte" | null;
  link_rssi_dbm: number | null;
  link_snr_db: number | null;
  source: string;
  recorded_at: string;
  received_at: string;
  schema_version: number;
}

export function positionToTelemetryDevice(row: PositionRow): TelemetryDevice {
  const recordedAt = Date.parse(row.recorded_at);
  const flags = row.flags;
  return {
    id: row.device_uid,
    name: `Device ${row.device_uid}`,
    seq: row.message_id,
    time: Math.floor(recordedAt / 1000),
    status: statusName(row.status_code),
    profile: profileName(row.power_profile_code),
    error: flags !== null && (flags & 0x80) !== 0 ? "Module" : "None",
    lat: row.latitude,
    lon: row.longitude,
    hasGps: flags === null || (flags & 0x01) !== 0,
    batt: row.battery_mv ?? 0,
    batteryPercent: row.battery === null ? null : clamp(row.battery, 0, 100),
    rssi: row.link_rssi_dbm,
    snr: row.link_snr_db,
    bleHome: flags !== null && (flags & 0x08) !== 0,
    cellular: row.link_type === "lte",
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
    nullableNumber(row.battery_mv) &&
    nullableNumber(row.status_code) &&
    nullableNumber(row.power_profile_code) &&
    nullableNumber(row.flags) &&
    nullableNumber(row.tx_reason) &&
    (row.link_type === null || row.link_type === "lora" || row.link_type === "lte") &&
    nullableNumber(row.link_rssi_dbm) &&
    nullableNumber(row.link_snr_db) &&
    typeof row.source === "string" &&
    typeof row.recorded_at === "string" &&
    typeof row.received_at === "string" &&
    typeof row.schema_version === "number" &&
    Number.isFinite(Date.parse(row.recorded_at))
  );
}

function statusName(value: number | null): TelemetryDevice["status"] {
  return (["Home", "Out", "Lost", "Error"] as const)[value ?? 1] ?? "Error";
}

function profileName(value: number | null): TelemetryDevice["profile"] {
  return (["PowerSave", "Normal", "Active", "Emergency Lost"] as const)[value ?? 1] ?? "Normal";
}

function nullableNumber(value: unknown) {
  return value === null || typeof value === "number";
}

function clamp(value: number, minimum: number, maximum: number) {
  return Math.min(maximum, Math.max(minimum, value));
}
