import assert from "node:assert/strict";
import test from "node:test";
import { followedDeviceAfterAction } from "./followState.ts";

test("jumping to another device clears the active follow", () => {
  assert.equal(followedDeviceAfterAction(1001, 1002, "jump"), null);
});

test("jumping to the followed device also changes to one-time navigation", () => {
  assert.equal(followedDeviceAfterAction(1001, 1001, "jump"), null);
});

test("following a device retains the existing single-device toggle", () => {
  assert.equal(followedDeviceAfterAction(null, 1002, "follow"), 1002);
  assert.equal(followedDeviceAfterAction(1001, 1002, "follow"), 1002);
  assert.equal(followedDeviceAfterAction(1002, 1002, "follow"), null);
});
