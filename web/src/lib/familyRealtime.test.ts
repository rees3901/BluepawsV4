import assert from "node:assert/strict";
import test from "node:test";
import { familyRealtimeTopic, nextFamilyAccessVersion } from "./familyRealtime.ts";

test("names a private Family channel with its access version", () => {
  assert.equal(familyRealtimeTopic("jones-family", 4), "household:jones-family:v4");
});

test("accepts only a newer access version for the same Family", () => {
  assert.equal(nextFamilyAccessVersion({ id: "jones-family", access_version: 5 }, "jones-family", 4), 5);
  assert.equal(nextFamilyAccessVersion({ id: "other-family", access_version: 5 }, "jones-family", 4), null);
  assert.equal(nextFamilyAccessVersion({ id: "jones-family", access_version: 4 }, "jones-family", 4), null);
});

test("rejects invalid channel versions", () => {
  assert.throws(() => familyRealtimeTopic("jones-family", 0));
});
