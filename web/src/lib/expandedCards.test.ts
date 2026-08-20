import assert from "node:assert/strict";
import test from "node:test";
import { nextExpandedDeviceCards } from "./expandedCards.ts";

test("expanding a collapsed card keeps already expanded cards open", () => {
  assert.deepEqual(nextExpandedDeviceCards([1001, 1002], 1003), [1001, 1002, 1003]);
});

test("expanding a fifth card closes the oldest expanded card", () => {
  assert.deepEqual(nextExpandedDeviceCards([1001, 1002, 1003, 1004], 1005), [1002, 1003, 1004, 1005]);
});

test("expanding an already open card closes only that card", () => {
  assert.deepEqual(nextExpandedDeviceCards([1001, 1002, 1003], 1002), [1001, 1003]);
});

