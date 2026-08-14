import assert from "node:assert/strict";
import test from "node:test";
import { EMPTY_MAP_CENTER, EMPTY_MAP_ZOOM } from "./mapViewport.ts";

test("the empty dashboard opens on a neutral whole-UK overview", () => {
  assert.deepEqual(EMPTY_MAP_CENTER, [54.5, -3.5]);
  assert.equal(EMPTY_MAP_ZOOM, 5);
});
