// Conservative advertising presets supported by the current ESP32 hub builds.
// Highest is the highest offered preset, not a universal chip power limit.
export const HUB_BLE_POWERS = [
  { value: -12, label: "Lowest — −12 dBm" },
  { value: -6, label: "Low — −6 dBm" },
  { value: 3, label: "Medium — +3 dBm" },
  { value: 6, label: "High — +6 dBm" },
  { value: 9, label: "Highest — +9 dBm" },
] as const;
export type HubBlePower = typeof HUB_BLE_POWERS[number]["value"];
export function isHubBlePower(value: unknown): value is HubBlePower {
  return HUB_BLE_POWERS.some(power => power.value === value);
}
export function supportsHubBlePower(value: unknown, steps: number | null | undefined) {
  return isHubBlePower(value) && (steps === 5 || value === -12 || value === 3 || value === 9);
}
