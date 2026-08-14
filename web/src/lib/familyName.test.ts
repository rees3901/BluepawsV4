import assert from "node:assert/strict";
import test from "node:test";
import { MAX_FAMILY_NAME_LENGTH, normalizeFamilyName } from "./familyName.ts";

test("normalizes a Family display name without changing its meaning", () => {
  assert.equal(normalizeFamilyName("  The Jones Family  "), "The Jones Family");
});

test("rejects empty and overlong Family display names", () => {
  assert.equal(normalizeFamilyName("   "), null);
  assert.equal(normalizeFamilyName("x".repeat(MAX_FAMILY_NAME_LENGTH + 1)), null);
});
