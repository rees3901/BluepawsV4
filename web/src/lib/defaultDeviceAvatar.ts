import type { DeviceAvatar } from "../types/telemetry";

const FALLBACK_EMOJIS = ["🐱", "🐶", "🐰", "🐾", "🦊", "🐹", "🦉", "🐼"];
const FALLBACK_COLORS = ["#1d9bf0", "#ff6b35", "#a855f7", "#22c55e", "#f97316", "#06b6d4", "#84cc16", "#ec4899"];

function stableIndex(deviceId: number, modulo: number) {
  return Math.abs(Math.trunc(deviceId)) % modulo;
}

export function defaultDeviceAvatar(deviceId: number): DeviceAvatar {
  return {
    kind: "emoji",
    emoji: FALLBACK_EMOJIS[stableIndex(deviceId, FALLBACK_EMOJIS.length)],
    color: FALLBACK_COLORS[stableIndex(deviceId, FALLBACK_COLORS.length)],
  };
}
