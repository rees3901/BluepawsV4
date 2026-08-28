export type CollarStatus = "Home" | "Out" | "Lost" | "Error";
export type PowerProfile = "Normal" | "PowerSave" | "Active" | "Emergency Lost" | "Debug";
export type TelemetryError = "None" | "GPS" | "RF" | "Cellular" | "Module";
export type IngestPath = "cellular_direct" | "lora_hub";

export interface TelemetryDevice {
  entity?: "hub";
  hubMode?: "home" | "portable" | "off_grid";
  hubReportingProfile?: "normal" | "power_save" | "active";
  id: number;
  name: string;
  seq: number;
  time: number;
  status: CollarStatus;
  profile: PowerProfile;
  error: TelemetryError;
  faultReport?: { flags: number; txReason?: number | null } | null;
  lat: number;
  lon: number;
  hasGps: boolean;
  homeHub?: { id: number; lat: number | null; lon: number | null; fixAt: string | null } | null;
  batt: number;
  batteryPercent?: number | null;
  rssi: number | null;
  snr: number | null;
  bleHome: boolean;
  ingestPath: IngestPath | null;
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
  kind: "emoji" | "photo";
  emoji: string;
  color: string;
  photoUrl?: string;
  storagePath?: string;
}

export type DeviceAction = "jump" | "follow" | "trail" | "find" | "command";
