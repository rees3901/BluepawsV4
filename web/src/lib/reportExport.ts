import type { DeviceReport } from "./deviceReports";
import type { HubPresence } from "./hubPresence";

export type ReportExportFormat = "json" | "csv";
export const REPORT_EXPORT_LIMIT = 1000;
const known = (value: unknown): value is number => typeof value === "number" && Number.isFinite(value);
const timestamp = (value: string) => Number.isFinite(Date.parse(value)) ? new Date(value).toISOString() : null;
const cell = (value: unknown) => {
  if (known(value)) return String(value);
  const text = value == null ? "" : String(value);
  return `"${(/^[\s]*[=+\-@]/.test(text) ? "'" + text : text).replaceAll('"', '""')}"`;
};
const csv = (records: Record<string, unknown>[]) => {
  const columns = Object.keys(records[0] ?? {});
  return "\uFEFF" + [columns, ...records.map(row => columns.map(key => row[key]))].map(row => row.map(cell).join(",")).join("\r\n");
};
function file(name: string, id: number, format: ReportExportFormat, now: number, content: unknown) {
  const safe = name.toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_|_$/g, "").slice(0, 60) || "device";
  return {
    filename: `bluepaws_${safe}_${id}_reports_${new Date(now).toISOString().replace(/[:.]/g, "-")}.${format}`,
    mimeType: format === "json" ? "application/json;charset=utf-8" : "text/csv;charset=utf-8",
    content: format === "json" ? JSON.stringify(content, null, 2) + "\n" : csv(content as Record<string, unknown>[]),
  };
}

function collarReading(report: DeviceReport) {
  const o = report.observation, path = report.path;
  const packet = o.packetAvailable !== false;
  const position = o.gnss_valid && known(o.latitude) && known(o.longitude) && Math.abs(o.latitude) <= 90 && Math.abs(o.longitude) <= 180;
  const fixAge = packet && position && Number.isInteger(o.fix_age_s) && o.fix_age_s >= 0 && o.fix_age_s < 65535 ? o.fix_age_s : null;
  const recorded = Date.parse(o.recorded_at), received = Date.parse(o.received_at);
  const full = packet && [0, 3, 4].includes(o.tx_reason);
  const measured = full && fixAge !== null && Number.isFinite(recorded) && Number.isFinite(received)
    ? Math.min(recorded, received) - fixAge * 1000 : null;
  return {
    device_id: o.device_guid16, observation_id: packet ? o.id : null, sequence: packet ? o.msg_seq_id : null,
    record_kind: packet ? "accepted_packet" : "dashboard_snapshot",
    received_at_utc: timestamp(o.received_at), recorded_at_utc: timestamp(o.recorded_at),
    report_type: packet ? ["Telemetry", "ACK", "Ping", "User request", "Boot report", "Alert", "Config", "Wake check-in"][o.tx_reason] ?? "Unknown" : "Dashboard snapshot",
    report_type_code: packet ? o.tx_reason : null,
    status: ["Home", "Out", "Lost", "Error"][o.status] ?? "Unknown", status_code: o.status,
    power_profile: ["PowerSave", "Normal", "Active", "Emergency Lost", "Debug"][o.power_profile] ?? "Unknown", power_profile_code: o.power_profile,
    latitude_deg: position ? o.latitude : null, longitude_deg: position ? o.longitude : null,
    gps_position_in_packet: packet ? position : null,
    gps_measured_at_utc: measured !== null && measured > 0 ? new Date(measured).toISOString() : null,
    gps_fix_age_at_report_s: fixAge,
    gps_accuracy_m: packet && position && known(o.acc_m) && o.acc_m > 0 && o.acc_m < 65535 ? o.acc_m : null,
    satellites: packet && position && Number.isInteger(o.sat_count) && o.sat_count >= 0 && o.sat_count < 255 ? o.sat_count : null,
    battery_mv: known(o.batt_mv) && o.batt_mv > 0 && o.batt_mv < 65535 ? o.batt_mv : null,
    radio_rssi_dbm: path?.link_rssi_dbm ?? null, radio_snr_db: path?.link_snr_db ?? null,
    ingest_path: path?.ingest_path ?? null, link_type: path?.link_type ?? null, gateway_id: path?.gateway_guid16 ?? null,
    receipt_count_on_selected_route: packet ? path?.receipt_count ?? null : null,
    home_beacon_seen_in_packet: packet ? !!(o.flags & 0x08) : null,
    stale_gps_flag_in_packet: packet ? !!(o.flags & 0x40) : null,
    error_flag_in_packet: packet ? !!(o.flags & 0x80) : null,
    raw_flags: packet ? o.flags : null,
  };
}

export function createCollarReportExport(device: { id: number; name: string }, reports: DeviceReport[], format: ReportExportFormat = "json", now = Date.now()) {
  if (!reports.length) throw new Error("No reports are available to export");
  if (reports.length > REPORT_EXPORT_LIMIT) throw new Error("Export exceeds the report limit");
  if (reports.some(report => report.observation.device_guid16 !== device.id)) throw new Error("Report identity does not match the selected collar. Reopen its message log and try again.");
  const ordered = [...reports].sort((a, b) => Date.parse(a.observation.received_at) - Date.parse(b.observation.received_at) || a.observation.id - b.observation.id);
  const readings = ordered.map(collarReading);
  const received = readings.map(row => row.received_at_utc).filter((value): value is string => value !== null).sort();
  const gaps = received.slice(1).map((time, index) => (Date.parse(time) - Date.parse(received[index])) / 1000);
  const count = (key: keyof typeof readings[number]) => readings.filter(row => row[key] === true).length;
  const payload = {
    schema: "bluepaws.report-history", schema_version: 1, exported_at_utc: new Date(now).toISOString(),
    device: { entity: "collar", id: device.id, name: device.name },
    coverage: { scope: "latest_available_reports", requested_max_reports: REPORT_EXPORT_LIMIT, exported_reports: reports.length, limit_reached: reports.length >= REPORT_EXPORT_LIMIT, oldest_received_at_utc: received[0] ?? null, newest_received_at_utc: received.at(-1) ?? null, order: "oldest_first" },
    summary: { accepted_packets: readings.filter(row => row.record_kind === "accepted_packet").length, dashboard_snapshots: readings.filter(row => row.record_kind === "dashboard_snapshot").length, packets_with_position: count("gps_position_in_packet"), packets_with_home_beacon: count("home_beacon_seen_in_packet"), packets_with_stale_gps_flag: count("stale_gps_flag_in_packet"), packets_with_error_flag: count("error_flag_in_packet"), largest_received_report_gap_s: gaps.length ? Math.max(...gaps) : null,
      report_types: Object.fromEntries([...new Set(readings.map(row => row.report_type))].map(type => [type, readings.filter(row => row.report_type === type).length])) },
    notes: ["UTC timestamps; unit suffixes identify numeric units. Null means unavailable, not zero.", "GPS measured time is derived only for full GPS reports from recorded time minus reported fix age, with future recorded time capped at receipt time. It is not a new live fix.", "Check-ins and ACKs confirm contact, not a new GPS position. Raw values and optional TLVs are preserved separately.", "Route receipts are repeat deliveries of an observation, not extra collar reports. Time gaps do not prove packet loss.", "Raw observations are database-decoded packets, not original radio byte frames. Dashboard snapshots are explicitly identified and contain no original packet."],
    reports: ordered.map((report, index) => ({ readings: readings[index], raw_observation: report.observation.packetAvailable === false ? null : report.observation, accepted_routes: report.observation.packetAvailable === false ? [] : report.paths ?? (report.path ? [report.path] : []) })),
  };
  return file(device.name, device.id, format, now, format === "csv" ? readings : payload);
}

export function createHubReportExport(hub: HubPresence, format: ReportExportFormat = "json", now = Date.now()) {
  // Deliberate allowlist: no household IDs, avatar paths or unrelated settings.
  const reading = { gateway_id: hub.gateway_guid16, received_at_utc: timestamp(hub.received_at), gps_measured_at_utc: hub.fix_at ? timestamp(hub.fix_at) : null,
    mode: hub.mode, reporting_profile: hub.reporting_profile ?? "normal", latitude_deg: hub.latitude, longitude_deg: hub.longitude,
    position_simulated: hub.position_simulated ?? null, battery_simulated: hub.battery_simulated ?? null,
    gnss_valid: hub.gnss_valid ?? null, gps_accuracy_m: hub.acc_m ?? null, satellites: hub.sat_count ?? null, hdop: hub.hdop ?? null,
    battery_percent: hub.battery_percent ?? null, wifi_rssi_dbm: hub.wifi_rssi_dbm, uptime_s: hub.uptime_s, free_heap_bytes: hub.free_heap,
    bluetooth_enabled: hub.ble_enabled, home_beacon_advertising: hub.ble_advertising, bluetooth_tx_power_dbm: hub.ble_tx_power_dbm ?? null,
    settings_revision: hub.settings_revision, applied_revision: hub.applied_revision };
  return file(hub.display_name, hub.gateway_guid16, format, now, format === "csv" ? [reading] : {
    schema: "bluepaws.hub-snapshot", schema_version: 1, exported_at_utc: new Date(now).toISOString(), device: { entity: "hub", id: hub.gateway_guid16, name: hub.display_name },
    coverage: { scope: "latest_hub_snapshot", exported_reports: 1 },
    notes: ["This is the latest hub status snapshot, not historical collar telemetry or a hub history archive.", "UTC timestamps; numeric units are identified in field names. Null means unavailable, not zero. Simulation flags distinguish testbed readings from measurements."],
    reports: [reading],
  });
}

export function saveReportExport(result: ReturnType<typeof createCollarReportExport>) {
  const url = URL.createObjectURL(new Blob([result.content], { type: result.mimeType }));
  const anchor = document.createElement("a");
  anchor.href = url; anchor.download = result.filename;
  document.body.append(anchor); anchor.click(); anchor.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
