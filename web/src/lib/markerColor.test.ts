import assert from "node:assert/strict";
import test from "node:test";
import { DEFAULT_MARKER_COLOR, normalizeMarkerColor } from "./markerColor.ts";

test("normalises a valid six-digit marker colour", () => {
  assert.equal(normalizeMarkerColor("#A855F7"), "#a855f7");
});

test("rejects values that cannot be safely embedded in a map marker", () => {
  assert.equal(normalizeMarkerColor("red;transform:rotate(2deg)"), DEFAULT_MARKER_COLOR);
  assert.equal(normalizeMarkerColor("#fff"), DEFAULT_MARKER_COLOR);
});
