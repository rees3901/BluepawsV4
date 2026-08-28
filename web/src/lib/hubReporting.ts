export type HubReportingProfile = "power_save" | "normal" | "active";
export const HUB_REPORTING = {
  power_save: { label: "Power Save", seconds: 180 },
  normal: { label: "Normal", seconds: 60 },
  active: { label: "Active", seconds: 30 },
} as const;
// Missing means legacy firmware, which reports once per minute.
export function hubReportInterval(profile?: HubReportingProfile) {
  return HUB_REPORTING[profile ?? "normal"].seconds;
}
export function hubContactGrace(profile?: HubReportingProfile) {
  return hubReportInterval(profile) + 30;
}
