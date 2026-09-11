import type { DeviceAction, DeviceAvatar, MapCommand, TelemetryDevice, TrailPoint } from "@/types/telemetry";
import type { MapLayerPickerName } from "@/lib/mapLayers";

export type MapRendererName = "leaflet" | "maplibre";
export type VectorSourceName = "online" | "pmtiles";

export interface MapViewport {
  latitude: number;
  longitude: number;
  zoom: number;
}

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
  onUserNavigation?: () => void;
  onNotice?: (message: string) => void;
  onViewportChange?: (viewport: MapViewport) => void;
  readOnly?: boolean;
}

export interface ConfiguredMapRendererProps extends MapRendererProps {
  rasterLayer: MapLayerPickerName;
  vectorSource: VectorSourceName;
}

export function locatedDevices(devices: TelemetryDevice[]) {
  return devices.filter(device =>
    (device.entity !== "hub" || device.hasGps)
    && Number.isFinite(device.lat)
    && Number.isFinite(device.lon));
}
