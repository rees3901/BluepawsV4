export type TrailLatLng = [number, number];
export const VISIBLE_TRAIL_POINT_LIMIT = 4;

export function appendTrailPoint(points: TrailLatLng[], nextPoint: TrailLatLng): TrailLatLng[] {
  const latestPoint = points.at(-1);
  if (latestPoint?.[0] === nextPoint[0] && latestPoint[1] === nextPoint[1]) return points;
  return [...points, nextPoint].slice(-VISIBLE_TRAIL_POINT_LIMIT);
}
