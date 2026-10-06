import type { TelemetryDevice, TrailPoint } from "../types/telemetry.ts";

export type TrailLatLng = [number, number];
export const VISIBLE_TRAIL_POINT_LIMIT = 4;
export const TRAIL_MAX_AGE_MS = 4 * 60 * 60 * 1000;

export function pruneTrailPoints(points: TrailPoint[], now: number): TrailPoint[] {
  return points.filter(point => {
    const timestamp = Date.parse(point.recordedAt);
    return Number.isFinite(timestamp) && timestamp <= now && now - timestamp <= TRAIL_MAX_AGE_MS;
  }).sort((a, b) => Date.parse(a.recordedAt) - Date.parse(b.recordedAt))
    .slice(-VISIBLE_TRAIL_POINT_LIMIT);
}

// Share exactly the same age/count rules between raster and vector renderers.
export function updateTrailPoints(cached: TrailPoint[], history: TrailPoint[], device: TelemetryDevice, now: number): TrailPoint[] {
  const recordedAt = device.positionRecordedAt === undefined
    ? (device.time > 0 ? new Date(device.time * 1000).toISOString() : null)
    : device.positionRecordedAt;
  const current = recordedAt ? [{ lat: device.lat, lon: device.lon, recordedAt }] : [];
  const merged = [...history, ...cached, ...current].filter(point => {
    const timestamp = Date.parse(point.recordedAt);
    return Number.isFinite(timestamp) && timestamp <= now && now - timestamp <= TRAIL_MAX_AGE_MS;
  }).sort((a, b) => Date.parse(a.recordedAt) - Date.parse(b.recordedAt));
  const points: TrailPoint[] = [];
  for (const point of merged) {
    const last = points.at(-1);
    if (last?.lat === point.lat && last.lon === point.lon) points[points.length - 1] = point;
    else points.push(point);
  }
  return pruneTrailPoints(points, now);
}

export function appendTrailPoint(points: TrailLatLng[], nextPoint: TrailLatLng): TrailLatLng[] {
  const latestPoint = points.at(-1);
  if (latestPoint?.[0] === nextPoint[0] && latestPoint[1] === nextPoint[1]) return points;
  return [...points, nextPoint].slice(-VISIBLE_TRAIL_POINT_LIMIT);
}
