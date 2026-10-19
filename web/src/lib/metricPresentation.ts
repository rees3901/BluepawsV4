function metricClass(value: number | null | undefined, amber: number, red: number) {
  if (value == null || !Number.isFinite(value) || value < 0) return "";
  return value > red ? "metric-red" : value > amber ? "metric-amber" : "";
}

// Use original readings, not rounded display strings. Unknown is not zero.
export const distanceMetricClass = (metres?: number | null) => metricClass(metres, 600, 1000);
export const contactMetricClass = (seconds?: number | null) => metricClass(seconds, 30 * 60, 2 * 60 * 60);
