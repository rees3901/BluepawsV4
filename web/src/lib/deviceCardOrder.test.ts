import assert from "node:assert/strict";
import test from "node:test";
import { deviceCardOrderStorageKey, deviceCardPinStorageKey, moveDeviceBefore, moveDeviceToHoverTarget, orderDeviceIds, pinDeviceFirst } from "./deviceCardOrder.ts";

test("orders visible devices using saved preference and appends new devices", () => {
  assert.deepEqual(orderDeviceIds([1001, 1002, 1003, 1004], [1003, 1001, 9999]), [1003, 1001, 1002, 1004]);
});

test("moves one device before another while preserving the rest", () => {
  assert.deepEqual(moveDeviceBefore([1001, 1002, 1003, 1004], 1004, 1002), [1001, 1004, 1002, 1003]);
});

test("pinning a device moves it to the top", () => {
  assert.deepEqual(pinDeviceFirst([1001, 1002, 1003], 1003), [1003, 1001, 1002]);
});

test("a persisted pin reserves the first slot", () => {
  assert.deepEqual(orderDeviceIds([1001, 1002, 1003], [1002, 1003, 1001], 1003), [1003, 1002, 1001]);
  assert.deepEqual(moveDeviceBefore([1003, 1002, 1001], 1001, 1003, 1003), [1003, 1001, 1002]);
  assert.deepEqual(moveDeviceBefore([1003, 1002, 1001], 1003, 1001, 1003), [1003, 1002, 1001]);
});

test("hover reordering moves cards visibly around the target without displacing a pin", () => {
  assert.deepEqual(moveDeviceToHoverTarget([1001, 1002, 1003], 1001, 1002), [1002, 1001, 1003]);
  assert.deepEqual(moveDeviceToHoverTarget([1001, 1002, 1003], 1003, 1002), [1001, 1003, 1002]);
  assert.deepEqual(moveDeviceToHoverTarget([1001, 1002, 1003], 1003, 1001, 1001), [1001, 1003, 1002]);
});

test("storage key is scoped to user and Family", () => {
  assert.equal(
    deviceCardOrderStorageKey("owner@example.com", "family-1"),
    "bp_device_card_order:owner@example.com:family-1",
  );
  assert.equal(
    deviceCardPinStorageKey("owner@example.com", "family-1"),
    "bp_device_card_pin:owner@example.com:family-1",
  );
});
