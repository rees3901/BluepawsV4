import type { DeviceReport } from "./deviceReports";
import type { HubPresence } from "./hubPresence";
import { HUB_REPORTING } from "./hubReporting.ts";

export function buildHubReport(hub: HubPresence): Pick<DeviceReport,"rows"|"summary"> {
  const mode = hub.mode === "home" ? "Home" : hub.mode === "portable" ? "Portable" : "Off-Grid";
  const received = new Date(hub.received_at).toLocaleString();
  const coordinates = hub.latitude === null || hub.longitude === null ? "Waiting for GPS fix" : `${hub.latitude.toFixed(5)}, ${hub.longitude.toFixed(5)}`;
  const values = [
    ["Received",received,"When Bluepaws accepted this hub’s own status report, not a collar report."],
    ["Report type","Hub self-report","The hub reports its own status at its selected reporting cadence while online."],
    ["Reporting profile",HUB_REPORTING[hub.reporting_profile ?? "normal"].label,
      `Reports approximately every ${HUB_REPORTING[hub.reporting_profile ?? "normal"].seconds} seconds. Reception and command handling stay on.`],
    ["Hub ID",hub.gateway_guid16.toString(16).padStart(4,"0"),"The identity of this Home Hub."],
    ["Mode",mode,"Home uses primary Wi-Fi; Portable uses a fallback connection; Off-Grid is local only."],
    ["Coordinates",coordinates,"The hub’s own last known GPS position, never a collar’s position."],
    ["GPS fix",hub.fix_at ? new Date(hub.fix_at).toLocaleString() : "Not acquired","When the position was measured; this may be older than the last contact."],
    ["Battery","No data","Battery telemetry is not supplied by this hub yet. This does not mean the battery is empty."],
    ["Wi-Fi signal",hub.wifi_rssi_dbm === null ? "Not connected" : `${hub.wifi_rssi_dbm} dBm`,"Wi-Fi signal strength: less negative values are stronger. This is not the collar’s RF link."],
    ["Home beacon",hub.ble_advertising ? "Advertising" : "Off","Whether the hub is currently advertising its BLE Home beacon."],
    ["Uptime",`${hub.uptime_s} seconds`,"How long the hub has been running since its last restart."],
  ];
  return {summary:`${hub.display_name} checked in in ${mode} mode at ${received}.`,
    rows:values.map(([field,data,description]) => ({field,data,description}))};
}

export function hubReportCsv(hub: HubPresence) {
  const report = buildHubReport(hub);
  // Quoted cells alone do not stop spreadsheet formula interpretation.
  const cell = (s: string) => `"${(/^[\s]*[=+\-@]/.test(s) ? "'"+s : s).replaceAll('"','""')}"`;
  return [["Field","Data","Description"], ["Summary",report.summary,"Latest hub self-report"],
    ...report.rows.map(r => [r.field,r.data,r.description])].map(row => row.map(cell).join(",")).join("\r\n");
}
