import type { TelemetryDevice } from "../types/telemetry.ts";

export const MAX_UNCERTAINTY_RADIUS_M = 50;
export function gnssUncertainty(device: Pick<TelemetryDevice, "entity" | "hasGps" | "status" | "lat" | "lon" | "gnss">) {
  const accuracy = device.gnss?.accuracyM;
  if (device.entity === "hub" || device.status === "Home" || !device.hasGps ||
      typeof accuracy !== "number" || !Number.isFinite(accuracy) || accuracy <= 0 || accuracy >= 65535 ||
      !Number.isFinite(device.lat) || !Number.isFinite(device.lon) || Math.abs(device.lat) > 90 || Math.abs(device.lon) > 180) return null;
  const radius = Math.min(accuracy, MAX_UNCERTAINTY_RADIUS_M);
  return { radius, capped: accuracy > radius,
    label: `Estimated GPS uncertainty: ${accuracy} m${accuracy > radius ? " — ring capped at 50 m" : ""}. At the last GPS fix; does not account for movement since then.` };
}

// Geographic polygon rather than a pixel-radius circle, so zoom keeps metres.
export function uncertaintyPolygon(lat: number, lon: number, radius: number) {
  const angular = radius / 6371000;
  const latitude = lat * Math.PI / 180, longitude = lon * Math.PI / 180;
  const coordinates: number[][] = [];
  for (let i = 0; i < 64; i++) {
    const bearing = i * 2 * Math.PI / 64;
    const nextLat = Math.asin(Math.sin(latitude) * Math.cos(angular) + Math.cos(latitude) * Math.sin(angular) * Math.cos(bearing));
    const nextLon = longitude + Math.atan2(Math.sin(bearing) * Math.sin(angular) * Math.cos(latitude), Math.cos(angular) - Math.sin(latitude) * Math.sin(nextLat));
    coordinates.push([nextLon * 180 / Math.PI, nextLat * 180 / Math.PI]);
  }
  coordinates.push([...coordinates[0]]);
  return { type: "Polygon" as const, coordinates: [coordinates] };
}
