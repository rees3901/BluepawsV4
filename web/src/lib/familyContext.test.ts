import assert from "node:assert/strict";
import test from "node:test";
import { resolveActiveFamily, type FamilyMembership } from "./familySelection.ts";

const families: FamilyMembership[] = [
  { householdId: "family-a", name: "The Jones Family", role: "owner", joinedAt: "2026-08-01T10:00:00Z" },
  { householdId: "family-b", name: "The Smith Family", role: "member", joinedAt: "2026-08-02T10:00:00Z" },
];

test("uses the explicitly selected active Family", () => {
  assert.equal(resolveActiveFamily(families, "family-b")?.householdId, "family-b");
});

test("falls back deterministically when the saved Family is unavailable", () => {
  assert.equal(resolveActiveFamily(families, "removed-family")?.householdId, "family-a");
});

test("returns no active Family for a user without memberships", () => {
  assert.equal(resolveActiveFamily([], null), null);
});
