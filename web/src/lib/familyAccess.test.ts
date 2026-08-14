import assert from "node:assert/strict";
import test from "node:test";
import { canRemoveFamilyMember } from "./familyAccess.ts";

test("an Owner can remove another Family Member", () => {
  assert.equal(canRemoveFamilyMember("owner", "owner-a", "member", "member-b"), true);
});

test("a Member cannot remove another Member", () => {
  assert.equal(canRemoveFamilyMember("member", "member-a", "member", "member-b"), false);
});

test("an Owner cannot remove an Owner or themselves", () => {
  assert.equal(canRemoveFamilyMember("owner", "owner-a", "owner", "owner-b"), false);
  assert.equal(canRemoveFamilyMember("owner", "owner-a", "member", "owner-a"), false);
});
