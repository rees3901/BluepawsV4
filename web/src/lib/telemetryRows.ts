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
  ingest_path: "cellular_direct" | "lora_hub" | null;
  link_type: "lora" | "lte" | null;
  link_rssi_dbm: number | null;
  link_snr_db: number | null;
  source: string;
  recorded_at: string;
  received_at: string;
  schema_version: number;
}

export interface DevicePresenceRow {
  device_id: number;
  household_id: string;
  last_seen_at: string | null;
  last_seen_status_code: number | null;
  last_seen_power_profile_code: number | null;
  last_seen_tx_reason: number | null;
  last_seen_battery_mv: number | null;
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
    ingestPath: row.ingest_path,
    lastUpdate: recordedAt,
    source: row.source,
  };
}

export function applyPresenceToTelemetryDevice(device: TelemetryDevice, row: DevicePresenceRow): TelemetryDevice {
  if (row.device_id !== device.id || row.household_id.length === 0 || !row.last_seen_at) return device;
  const lastSeenAt = Date.parse(row.last_seen_at);
  if (!Number.isFinite(lastSeenAt) || lastSeenAt <= device.lastUpdate) return device;

  const hasFreshBattery = row.last_seen_battery_mv !== null;
  return {
    ...device,
    time: Math.floor(lastSeenAt / 1000),
    status: row.last_seen_status_code === null ? device.status : statusName(row.last_seen_status_code),
    profile: row.last_seen_power_profile_code === null ? device.profile : profileName(row.last_seen_power_profile_code),
    batt: row.last_seen_battery_mv ?? device.batt,
    batteryPercent: hasFreshBattery ? null : device.batteryPercent,
    bleHome: device.bleHome || row.last_seen_tx_reason === 7 || row.last_seen_status_code === 0,
    lastUpdate: lastSeenAt,
    source: row.last_seen_tx_reason === 7 ? "tlv-wake-checkin" : device.source,
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
    (row.ingest_path === null || row.ingest_path === "cellular_direct" || row.ingest_path === "lora_hub") &&
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

export function isDevicePresenceRow(value: unknown): value is DevicePresenceRow {
  if (!value || typeof value !== "object") return false;
  const row = value as Record<string, unknown>;
  return (
    typeof row.device_id === "number" &&
    typeof row.household_id === "string" &&
    (row.last_seen_at === null || (typeof row.last_seen_at === "string" && Number.isFinite(Date.parse(row.last_seen_at)))) &&
    nullableNumber(row.last_seen_status_code) &&
    nullableNumber(row.last_seen_power_profile_code) &&
    nullableNumber(row.last_seen_tx_reason) &&
    nullableNumber(row.last_seen_battery_mv)
  );
}

function statusName(value: number | null): TelemetryDevice["status"] {
  return (["Home", "Out", "Lost", "Error"] as const)[value ?? 1] ?? "Error";
}

function profileName(value: number | null): TelemetryDevice["profile"] {
  return (["PowerSave", "Normal", "Active", "Emergency Lost", "Debug"] as const)[value ?? 1] ?? "Normal";
}

function nullableNumber(value: unknown) {
  return value === null || typeof value === "number";
}

function clamp(value: number, minimum: number, maximum: number) {
  return Math.min(maximum, Math.max(minimum, value));
}
