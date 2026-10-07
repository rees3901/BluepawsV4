// Conservative advertising presets supported by the current ESP32 hub builds.
// Max is the highest offered preset, not a universal chip power limit.
export const HUB_BLE_POWERS = [
  { value: -12, label: "Min — −12 dBm" },
  { value: 3, label: "Medium — +3 dBm" },
  { value: 9, label: "Max — +9 dBm" },
] as const;
export type HubBlePower = typeof HUB_BLE_POWERS[number]["value"];
export function isHubBlePower(value: unknown): value is HubBlePower {
  return HUB_BLE_POWERS.some(power => power.value === value);
}
