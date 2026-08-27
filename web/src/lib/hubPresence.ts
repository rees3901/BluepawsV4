import type { DeviceAvatar, TelemetryDevice } from "../types/telemetry";
export interface HubPresence {
  gateway_guid16: number; household_id: string; mode: "home" | "portable" | "off_grid";
  received_at: string; latitude: number | null; longitude: number | null; fix_at: string | null;
  uptime_s: number; wifi_rssi_dbm: number | null; ble_enabled: boolean; ble_advertising: boolean;
  free_heap: number; display_name: string; home_emoji: string; portable_emoji: string;
  marker_colour: string; desired_ble_enabled: boolean; settings_revision: number; applied_revision: number;
}
export function hubAvatar(hub: HubPresence): DeviceAvatar {
  return { kind: "emoji", emoji: hub.mode === "home" ? hub.home_emoji || "🏡" : hub.portable_emoji || "📱", color: hub.marker_colour };
}
export function hubMapDevice(hub: HubPresence): TelemetryDevice {
  return { id: -hub.gateway_guid16, name: hub.display_name, entity: "hub", hubMode: hub.mode,
    lat: hub.latitude ?? 0, lon: hub.longitude ?? 0, hasGps: hub.latitude !== null && hub.longitude !== null,
    lastUpdate: Date.parse(hub.received_at), seq: 0, time: 0, status: hub.mode === "home" ? "Home" : "Out",
    profile: "Normal", error: "None", batt: 0, rssi: hub.wifi_rssi_dbm, snr: null, bleHome: hub.ble_advertising,
    ingestPath: null, source: hub.fix_at ? "Hub GNSS · " + new Date(hub.fix_at).toLocaleString() : "No hub GPS fix yet" };
}
