// Product display bands for reported accuracy, not satellite RF signal strength.
export function gnssQuality(accuracyM: number | null | undefined, hasFix = true) {
  if (!hasFix) return { level: 0, label: "No fix", color: "#607d8b" };
  if (accuracyM == null || !Number.isFinite(accuracyM) || accuracyM <= 0 || accuracyM >= 65535) {
    return { level: 0, label: "Unknown", color: "#607d8b" };
  }
  if (accuracyM <= 5) return { level: 5, label: "Excellent", color: "#22c55e" };
  if (accuracyM <= 10) return { level: 4, label: "Good", color: "#84cc16" };
  if (accuracyM <= 25) return { level: 3, label: "Fair", color: "#f59e0b" };
  if (accuracyM <= 50) return { level: 2, label: "Poor", color: "#f97316" };
  return { level: 1, label: "Very poor", color: "#ef4444" };
}
