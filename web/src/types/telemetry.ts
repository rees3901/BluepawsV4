export type CollarStatus = "Home" | "Out" | "Lost" | "Error";
export type PowerProfile = "Normal" | "PowerSave" | "Active" | "Active Find" | "Emergency Lost";
export type TelemetryError = "None" | "GPS" | "RF" | "Cellular" | "Module";

export const VISIBLE_TRAIL_POINT_LIMIT = 4;

export interface TelemetryDevice {
  id: number;
  name: string;
  seq: number;
  time: number;
  status: CollarStatus;
  profile: PowerProfile;
  error: TelemetryError;
  lat: number;
  lon: number;
  hasGps: boolean;
  batt: number;
  batteryPercent?: number | null;
  rssi: number | null;
  snr: number | null;
  bleHome: boolean;
  cellular: boolean;
  lastUpdate: number;
  source?: string | null;
}

export interface TelemetrySource {
  subscribe(
    listener: (devices: TelemetryDevice[]) => void,
    statusListener?: (status: TelemetryConnectionStatus, detail?: string) => void,
  ): () => void;
}

export type TelemetryConnectionStatus = "connecting" | "connected" | "degraded";

export interface TrailPoint {
  lat: number;
  lon: number;
  recordedAt: string;
}

export interface MapCommand {
  type: "jump" | "fit";
  deviceId?: number;
  nonce: number;
}

export interface DeviceAvatar {
  emoji: string;
  color: string;
}

export type DeviceAction = "jump" | "follow" | "trail" | "find" | "command";
