import assert from "node:assert/strict";
import test from "node:test";
import { defaultExpandedDeviceCards, initialiseExpandedDeviceCards, nextExpandedDeviceCards } from "./expandedCards.ts";

const hub = { id: -1, entity: "hub" as const };
const pets = [{ id: 1001 }, { id: 1002 }, { id: 1003 }];

test("empty household and hub-only defaults", () => {
  assert.deepEqual(defaultExpandedDeviceCards([]), []);
  assert.deepEqual(defaultExpandedDeviceCards([hub]), [-1]);
});

test("one or two pets start expanded, without counting the hub", () => {
  for (const count of [1, 2]) {
    assert.deepEqual(defaultExpandedDeviceCards(pets.slice(0, count)), pets.slice(0, count).map(pet => pet.id));
    assert.deepEqual(defaultExpandedDeviceCards([hub, ...pets.slice(0, count)]), [-1, ...pets.slice(0, count).map(pet => pet.id)]);
  }
});

test("three or more pets start collapsed while the hub stays expanded", () => {
  assert.deepEqual(defaultExpandedDeviceCards(pets), []);
  assert.deepEqual(defaultExpandedDeviceCards([hub, ...pets]), [-1]);
  assert.deepEqual(defaultExpandedDeviceCards([hub, ...pets, { id: 1004 }]), [-1]);
});

test("delayed hub loading preserves manual pet choices", () => {
  assert.deepEqual(initialiseExpandedDeviceCards([1002], [1001, 1002], [hub, ...pets.slice(0, 2)]), [1002, -1]);
});

test("telemetry refresh does not reopen manually collapsed cards", () => {
  assert.deepEqual(initialiseExpandedDeviceCards([], [-1, 1001], [hub, pets[0]]), []);
});

test("pets arriving after the hub use the household size defaults", () => {
  assert.deepEqual(initialiseExpandedDeviceCards([-1], [-1], [hub, ...pets.slice(0, 2)]), [-1, 1001, 1002]);
  assert.deepEqual(initialiseExpandedDeviceCards([-1], [-1], [hub, ...pets]), [-1]);
});

test("expanding a collapsed card keeps already expanded cards open", () => {
  assert.deepEqual(nextExpandedDeviceCards([1001, 1002], 1003), [1001, 1002, 1003]);
});

test("expanding a fifth card closes the oldest expanded card", () => {
  assert.deepEqual(nextExpandedDeviceCards([1001, 1002, 1003, 1004], 1005), [1002, 1003, 1004, 1005]);
});

test("expanding an already open card closes only that card", () => {
  assert.deepEqual(nextExpandedDeviceCards([1001, 1002, 1003], 1002), [1001, 1003]);
});

