import test from "node:test";
import assert from "node:assert/strict";
import { normalizeDeviceName } from "./deviceName.ts";
test("friendly names trim whitespace without changing punctuation or Unicode", () => {
  assert.equal(normalizeDeviceName("  Mittens & O'Paws 🐈  "), "Mittens & O'Paws 🐈");
  assert.equal(normalizeDeviceName("x".repeat(80)).length, 80);
});
test("blank, overlong and control-character names are rejected", () => {
  for (const name of ["", "  ", "x".repeat(81), "Mittens\nPaws", "\u0000Mittens"])
    assert.throws(() => normalizeDeviceName(name));
});
test("hub names enforce firmware UTF-8 storage budget", () => {
  assert.equal(normalizeDeviceName("🏡".repeat(16), true), "🏡".repeat(16));
  assert.throws(() => normalizeDeviceName("🏡".repeat(17), true), /64 UTF-8/);
});
