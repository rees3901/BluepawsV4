import { createClient } from "@/lib/supabase/client";
import { formatMapCoordinates, googleMapsUrl } from "@/lib/mapLocation";
import type { CollarStatus, IngestPath, PowerProfile, TelemetryDevice } from "@/types/telemetry";

const REPORT_LIMIT = 10;

export interface DeviceObservationRow {
  id: number;
  device_guid16: number;
  msg_seq_id: number;
  time_unix: number;
  recorded_at: string;
  status: number;
  power_profile: number;
  flags: number;
  tx_reason: number;
  gnss_valid: boolean;
  latitude: number | null;
  longitude: number | null;
  batt_mv: number;
  acc_m: number;
  fix_age_s: number;
  sat_count: number;
  received_at: string;
}

export interface DeviceObservationPathRow {
  observation_id: number;
  ingest_path: IngestPath;
  link_type: "lora" | "lte";
  gateway_guid16: number | null;
  link_rssi_dbm: number | null;
  link_snr_db: number | null;
  first_received_at: string;
  last_received_at: string;
  receipt_count: number;
}

export interface DeviceReport {
  observation: DeviceObservationRow;
  path: DeviceObservationPathRow | null;
  rows: DeviceReportTableRow[];
  summary: string;
}

export interface DeviceReportTableRow {
  field: string;
  data: string;
  description: string;
}

export async function loadDeviceReports(deviceId: number): Promise<DeviceReport[]> {
  const supabase = createClient();
  const { data: observations, error } = await supabase
    .from("observations")
    .select("id,device_guid16,msg_seq_id,time_unix,recorded_at,status,power_profile,flags,tx_reason,gnss_valid,latitude,longitude,batt_mv,acc_m,fix_age_s,sat_count,received_at")
    .eq("device_guid16", deviceId)
    .order("received_at", { ascending: false })
    .limit(REPORT_LIMIT);

  if (error) throw error;

  const validObservations = ((observations ?? []) as unknown[]).filter(isDeviceObservationRow);
  if (validObservations.length === 0) return [];

  const observationIds = validObservations.map((row) => row.id);
  const { data: paths, error: pathError } = await supabase
    .from("observation_paths")
    .select("observation_id,ingest_path,link_type,gateway_guid16,link_rssi_dbm,link_snr_db,first_received_at,last_received_at,receipt_count")
    .in("observation_id", observationIds)
    .order("last_received_at", { ascending: false });

  if (pathError) {
    console.warn("Unable to load accepted report path details from Supabase", pathError);
    return validObservations.map((observation: DeviceObservationRow) => buildDeviceReport(observation, null));
  }

  const pathsByObservation = new Map<number, DeviceObservationPathRow>();
  ((paths ?? []) as unknown[]).filter(isDeviceObservationPathRow).forEach((row) => {
    if (!pathsByObservation.has(row.observation_id)) pathsByObservation.set(row.observation_id, row);
  });

  return validObservations.map((observation: DeviceObservationRow) => {
    const path = pathsByObservation.get(observation.id) ?? null;
    return buildDeviceReport(observation, path);
  });
}

export function buildCurrentDeviceReport(device: TelemetryDevice): DeviceReport {
  const observation: DeviceObservationRow = {
    id: 0,
    device_guid16: device.id,
    msg_seq_id: device.seq,
    time_unix: device.time,
    recorded_at: new Date(device.time * 1000).toISOString(),
    status: statusCode(device.status),
    power_profile: profileCode(device.profile),
    flags: device.bleHome ? 0x08 : device.hasGps ? 0x01 : 0,
    tx_reason: device.source === "tlv-wake-checkin" ? 7 : 0,
    gnss_valid: device.hasGps,
    latitude: device.hasGps ? device.lat : null,
    longitude: device.hasGps ? device.lon : null,
    batt_mv: device.batt,
    acc_m: 0,
    fix_age_s: 0,
    sat_count: 0,
    received_at: new Date(device.lastUpdate).toISOString(),
  };
  const path: DeviceObservationPathRow | null = device.ingestPath
    ? {
        observation_id: 0,
        ingest_path: device.ingestPath,
        link_type: device.ingestPath === "lora_hub" ? "lora" : "lte",
        gateway_guid16: null,
        link_rssi_dbm: device.rssi,
        link_snr_db: device.snr,
        first_received_at: new Date(device.lastUpdate).toISOString(),
        last_received_at: new Date(device.lastUpdate).toISOString(),
        receipt_count: 1,
      }
    : null;
  return buildDeviceReport(observation, path);
}

export function buildDeviceReport(observation: DeviceObservationRow, path: DeviceObservationPathRow | null): DeviceReport {
  const reportType = txReasonLabel(observation.tx_reason);
  const status = statusLabel(observation.status);
  const profile = profileLabel(observation.power_profile);
  const signal = signalLabel(path?.link_rssi_dbm ?? null, path?.link_snr_db ?? null);
  const source = sourceLabel(path);
  const receivedAt = formatDateTime(observation.received_at);
  const positionData = observation.gnss_valid && observation.latitude !== null && observation.longitude !== null
    ? formatMapCoordinates(observation.latitude, observation.longitude)
    : "No fresh GPS fix";

  const rows: DeviceReportTableRow[] = [
    {
      field: "Received",
      data: receivedAt,
      description: "When Bluepaws accepted this report from the collar or Home Hub.",
    },
    {
      field: "Report type",
      data: reportType,
      description: txReasonDescription(observation.tx_reason),
    },
    {
      field: "Status",
      data: status,
      description: statusDescription(observation.status),
    },
    {
      field: "Power profile",
      data: profile,
      description: profileDescription(observation.power_profile),
    },
    {
      field: "Position",
      data: positionData,
      description: observation.gnss_valid
        ? "This report included a fresh GPS position."
        : "This report did not include a new GPS position, so the map should keep the last valid location.",
    },
    {
      field: "Battery",
      data: `${observation.batt_mv} mV`,
      description: batteryDescription(observation.batt_mv),
    },
    {
      field: "GPS detail",
      data: observation.gnss_valid
        ? `${observation.acc_m} m accuracy · ${observation.fix_age_s}s fix age · ${observation.sat_count} satellites`
        : "No GPS fix in this report",
      description: observation.gnss_valid
        ? "Extra GPS quality details from the accepted collar report."
        : "Expected for a home wake check-in or an indoor collar where GNSS was skipped or unavailable.",
    },
    {
      field: "Signal",
      data: signal,
      description: signalDescription(path?.link_type ?? null),
    },
    {
      field: "Source",
      data: source,
      description: path?.ingest_path === "lora_hub"
        ? "The collar sent LoRa to the Home Hub, which relayed it to Bluepaws."
        : path?.ingest_path === "cellular_direct"
          ? "The collar sent this directly over cellular data."
          : "Bluepaws has no transport route recorded for this report.",
    },
    {
      field: "Sequence",
      data: String(observation.msg_seq_id),
      description: "A small rolling counter from the collar that helps identify new and duplicate reports.",
    },
    {
      field: "Accepted routes",
      data: path === null ? "Not reported" : String(path.receipt_count),
      description: path === null
        ? "No transport-route record was available for this report."
        : "How many times this same authenticated collar packet has been seen on this route.",
    },
  ];

  return {
    observation,
    path,
    rows,
    summary: `Device ${observation.device_guid16} ${summaryVerb(observation.tx_reason)} with status ${status.toLowerCase()} at ${receivedAt}.`,
  };
}

export function deviceReportsToCsv(reports: DeviceReport[]) {
  const lines = [
    [
      "received_at",
      "device_id",
      "sequence",
      "report_type",
      "status",
      "power_profile",
      "position",
      "battery_mv",
      "signal",
      "source",
      "summary",
    ],
    ...reports.map((report) => [
      report.observation.received_at,
      String(report.observation.device_guid16),
      String(report.observation.msg_seq_id),
      txReasonLabel(report.observation.tx_reason),
      statusLabel(report.observation.status),
      profileLabel(report.observation.power_profile),
      report.observation.gnss_valid && report.observation.latitude !== null && report.observation.longitude !== null
        ? googleMapsUrl(report.observation.latitude, report.observation.longitude)
        : "No fresh GPS fix",
      String(report.observation.batt_mv),
      signalLabel(report.path?.link_rssi_dbm ?? null, report.path?.link_snr_db ?? null),
      sourceLabel(report.path),
      report.summary,
    ]),
  ];
  return lines.map((line) => line.map(csvCell).join(",")).join("\n");
}

function isDeviceObservationRow(value: unknown): value is DeviceObservationRow {
  if (!value || typeof value !== "object") return false;
  const row = value as Record<string, unknown>;
  return (
    typeof row.id === "number" &&
    typeof row.device_guid16 === "number" &&
    typeof row.msg_seq_id === "number" &&
    typeof row.time_unix === "number" &&
    typeof row.recorded_at === "string" &&
    typeof row.status === "number" &&
    typeof row.power_profile === "number" &&
    typeof row.flags === "number" &&
    typeof row.tx_reason === "number" &&
    typeof row.gnss_valid === "boolean" &&
    (row.latitude === null || typeof row.latitude === "number") &&
    (row.longitude === null || typeof row.longitude === "number") &&
    typeof row.batt_mv === "number" &&
    typeof row.acc_m === "number" &&
    typeof row.fix_age_s === "number" &&
    typeof row.sat_count === "number" &&
    typeof row.received_at === "string"
  );
}

function isDeviceObservationPathRow(value: unknown): value is DeviceObservationPathRow {
  if (!value || typeof value !== "object") return false;
  const row = value as Record<string, unknown>;
  return (
    typeof row.observation_id === "number" &&
    (row.ingest_path === "lora_hub" || row.ingest_path === "cellular_direct") &&
    (row.link_type === "lora" || row.link_type === "lte") &&
    (row.gateway_guid16 === null || typeof row.gateway_guid16 === "number") &&
    (row.link_rssi_dbm === null || typeof row.link_rssi_dbm === "number") &&
    (row.link_snr_db === null || typeof row.link_snr_db === "number") &&
    typeof row.first_received_at === "string" &&
    typeof row.last_received_at === "string" &&
    typeof row.receipt_count === "number"
  );
}

function formatDateTime(value: string) {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;
  return new Intl.DateTimeFormat("en-GB", {
    dateStyle: "medium",
    timeStyle: "short",
  }).format(date);
}

function txReasonLabel(value: number) {
  return ([
    "Telemetry",
    "Timer",
    "Movement",
    "User request",
    "Low battery",
    "Recovery",
    "Debug",
    "Wake check-in",
  ] as const)[value] ?? `Unknown (${value})`;
}

function txReasonDescription(value: number) {
  if (value === 7) return "The collar woke briefly to touch base and prove it is still alive. This is most likely a home heartbeat rather than a full position update.";
  if (value === 0) return "A normal collar report, usually carrying the latest telemetry and position when GPS is available.";
  if (value === 4) return "The collar is reporting because battery level needs attention.";
  if (value === 6) return "A diagnostic report used during testing or troubleshooting.";
  return "Why the collar decided to send this report.";
}

function statusLabel(value: number) {
  return (["Home", "Out", "Lost", "Error"] as const)[value] ?? `Unknown (${value})`;
}

function statusDescription(value: number) {
  if (value === 0) return "The collar believes the pet is at home, usually because the Home Hub BLE beacon was seen.";
  if (value === 1) return "The collar believes the pet is away from the home beacon area.";
  if (value === 2) return "The pet has been marked as lost or is in a lost/search state.";
  return "The collar reported a problem or unusual condition.";
}

function profileLabel(value: number) {
  return (["Power Save", "Normal", "Active", "Lost Alert"] as const)[value] ?? `Unknown (${value})`;
}

function profileDescription(value: number) {
  if (value === 0) return "Battery-conserving mode with less frequent reporting.";
  if (value === 1) return "Everyday tracking mode for normal collar use.";
  if (value === 2) return "More frequent reporting for closer monitoring.";
  return "Emergency search mode with aggressive reporting at the cost of battery life.";
}

function batteryDescription(millivolts: number) {
  if (millivolts >= 4000) return "Healthy lithium battery voltage.";
  if (millivolts >= 3700) return "Normal working battery voltage.";
  if (millivolts >= 3400) return "Battery is getting low; charging should be considered.";
  return "Low battery voltage; the collar may soon reduce activity or need charging.";
}

function signalLabel(rssi: number | null, snr: number | null) {
  if (rssi === null && snr === null) return "Not reported";
  const quality = signalQuality(rssi, snr);
  const parts = [quality];
  if (rssi !== null) parts.push(`${Math.round(rssi)} dBm`);
  if (snr !== null) parts.push(`${snr.toFixed(1)} dB SNR`);
  return parts.join(" · ");
}

function signalQuality(rssi: number | null, snr: number | null) {
  if ((rssi !== null && rssi >= -90) || (snr !== null && snr >= 8)) return "Good";
  if ((rssi !== null && rssi >= -110) || (snr !== null && snr >= 0)) return "Average";
  return "Poor";
}

function signalDescription(linkType: "lora" | "lte" | null) {
  if (linkType === "lora") return "LoRa radio quality as measured by the Home Hub when it heard the collar.";
  if (linkType === "lte") return "Cellular link quality from the collar's mobile data path.";
  return "No radio or cellular signal quality was attached to this report.";
}

function sourceLabel(path: DeviceObservationPathRow | null) {
  if (path?.ingest_path === "lora_hub") return path.gateway_guid16 === null ? "Home Hub LoRa" : `Home Hub LoRa · ${path.gateway_guid16.toString(16).padStart(4, "0").toUpperCase()}`;
  if (path?.ingest_path === "cellular_direct") return "Cellular direct";
  return "Unknown";
}

function summaryVerb(txReason: number) {
  if (txReason === 7) return "checked in";
  if (txReason === 4) return "reported low battery";
  if (txReason === 6) return "sent a diagnostic report";
  return "sent a report";
}

function statusCode(value: CollarStatus) {
  return { Home: 0, Out: 1, Lost: 2, Error: 3 }[value] ?? 3;
}

function profileCode(value: PowerProfile) {
  if (value === "PowerSave") return 0;
  if (value === "Active" || value === "Active Find") return 2;
  if (value === "Emergency Lost") return 3;
  return 1;
}

function csvCell(value: string) {
  return `"${value.replaceAll('"', '""')}"`;
}
