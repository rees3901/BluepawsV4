import type { DeviceAction, DeviceAvatar, MapCommand, TelemetryDevice, TrailPoint } from "@/types/telemetry";

export type MapRendererName = "leaflet" | "maplibre";

export interface MapRendererProps {
  devices: TelemetryDevice[];
  avatars: Record<number, DeviceAvatar>;
  presenceNow: number;
  sidebarOpen: boolean;
  followedId: number | null;
  trailIds: Set<number>;
  trailHistory: Record<number, TrailPoint[]>;
  allTrailsVisible?: boolean;
  trailsAvailable?: boolean;
  command: MapCommand | null;
  onAction: (device: TelemetryDevice, action: DeviceAction) => void;
  onAllTrailsToggle?: () => void;
  onNotice?: (message: string) => void;
  readOnly?: boolean;
}

export function locatedDevices(devices: TelemetryDevice[]) {
  return devices.filter(device =>
    (device.entity !== "hub" || device.hasGps)
    && Number.isFinite(device.lat)
    && Number.isFinite(device.lon));
}

