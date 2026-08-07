import type { TelemetryDevice, TelemetrySource } from "@/types/telemetry";

type SeedDevice = Omit<TelemetryDevice, "seq" | "time" | "lastUpdate" | "error">;

const seedDevices: SeedDevice[] = [
  { id: 0x0001, name: "Whiskers", lat: 51.5055, lon: -0.09, status: "Out", profile: "Normal", batt: 4050, rssi: -86, snr: 7.4, hasGps: true, bleHome: false, cellular: false },
  { id: 0x0002, name: "Mittens", lat: 51.504, lon: -0.088, status: "Home", profile: "PowerSave", batt: 3900, rssi: -103, snr: 8.1, hasGps: true, bleHome: true, cellular: false },
  { id: 0x0003, name: "Shadow", lat: 51.507, lon: -0.092, status: "Out", profile: "Active", batt: 3750, rssi: -98, snr: 4.7, hasGps: true, bleHome: false, cellular: false },
  { id: 0x0004, name: "Patches", lat: 51.503, lon: -0.086, status: "Out", profile: "Normal", batt: 4100, rssi: -108, snr: 2.6, hasGps: true, bleHome: false, cellular: false },
  { id: 0x0005, name: "Luna", lat: 51.5062, lon: -0.094, status: "Home", profile: "Normal", batt: 3500, rssi: -82, snr: 9.2, hasGps: true, bleHome: true, cellular: false },
];

function randomDrift() {
  return (Math.random() - 0.5) * 0.0004;
}

function initialDevices(): TelemetryDevice[] {
  const now = Date.now();
  return seedDevices.map((device, index) => ({
    ...device,
    seq: index + 1,
    time: Math.floor(now / 1000),
    lastUpdate: now,
    error: "None",
  }));
}

/**
 * Development source used for parity work. The future Supabase source will
 * implement this same interface and subscribe to Realtime database changes.
 */
export const mockTelemetrySource: TelemetrySource = {
  subscribe(listener) {
    let devices = initialDevices();
    let sequence = devices.length;
    listener(devices);

    const timer = window.setInterval(() => {
      const index = Math.floor(Math.random() * devices.length);
      const now = Date.now();
      sequence += 1;
      devices = devices.map((device, deviceIndex) => {
        if (deviceIndex !== index) return device;
        return {
          ...device,
          seq: sequence,
          time: Math.floor(now / 1000),
          lastUpdate: now,
          lat: device.lat + randomDrift(),
          lon: device.lon + randomDrift(),
          batt: Math.max(3300, Math.min(4200, device.batt + Math.round(Math.random() * 18 - 9))),
          rssi: Math.max(-125, Math.min(-70, device.rssi + Math.round(Math.random() * 10 - 5))),
          snr: Math.max(-8, Math.min(12, Number((device.snr + Math.random() * 2 - 1).toFixed(1)))),
        };
      });
      listener(devices);
    }, 2000);

    return () => window.clearInterval(timer);
  },
};
