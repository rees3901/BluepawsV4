import assert from "node:assert/strict";
import test from "node:test";
import { appendTrailPoint, type TrailLatLng } from "./trailPoints.ts";

test("retains only the four newest positions after a long sequence", () => {
  let points: TrailLatLng[] = [];

  for (let index = 0; index < 100; index += 1) {
    points = appendTrailPoint(points, [index, -index]);
  }

  assert.deepEqual(points, [
    [96, -96],
    [97, -97],
    [98, -98],
    [99, -99],
  ]);
});

test("does not add the same reported coordinate twice", () => {
  const points: TrailLatLng[] = [[51.5, -0.1]];

  assert.strictEqual(appendTrailPoint(points, [51.5, -0.1]), points);
});
