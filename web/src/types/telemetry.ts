export type CollarStatus = "Home" | "Out" | "Lost" | "Error";
export type PowerProfile = "Normal" | "PowerSave" | "Active" | "Active Find" | "Emergency Lost";
export type TelemetryError = "None" | "GPS" | "RF" | "Cellular" | "Module";

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
  rssi: number;
  snr: number;
  bleHome: boolean;
  cellular: boolean;
  lastUpdate: number;
}

export interface TelemetrySource {
  subscribe(listener: (devices: TelemetryDevice[]) => void): () => void;
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
