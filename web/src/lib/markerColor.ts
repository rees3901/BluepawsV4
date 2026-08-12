export const DEFAULT_MARKER_COLOR = "#1d9bf0";

const HEX_COLOUR = /^#[0-9a-f]{6}$/i;

export function normalizeMarkerColor(value: string) {
  return HEX_COLOUR.test(value) ? value.toLowerCase() : DEFAULT_MARKER_COLOR;
}
