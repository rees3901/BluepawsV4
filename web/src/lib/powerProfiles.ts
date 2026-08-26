export type CustomerPowerProfile = "normal" | "power_save" | "active" | "lost_alert";

export const CUSTOMER_POWER_PROFILES: ReadonlyArray<{ value: CustomerPowerProfile; label: string }> = [
  { value: "normal", label: "Normal" },
  { value: "power_save", label: "Power Save" },
  { value: "active", label: "Active" },
  { value: "lost_alert", label: "Emergency Lost" },
];

export function powerProfileLabel(profile: CustomerPowerProfile) {
  return CUSTOMER_POWER_PROFILES.find((option) => option.value === profile)?.label ?? profile;
}
