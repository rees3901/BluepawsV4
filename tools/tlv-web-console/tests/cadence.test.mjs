import assert from "node:assert/strict";
import test from "node:test";
import { cadenceDelaySeconds, formatCadenceSeconds, initialCadenceDelaySeconds } from "../public/cadence.js";

test("cadence variance produces the configured lower, midpoint and upper bounds", () => {
  const settings = { reportCadenceSeconds: 65, reportVarianceSeconds: 30 };
  assert.equal(cadenceDelaySeconds(settings, () => 0), 35);
  assert.equal(cadenceDelaySeconds(settings, () => 0.5), 65);
  assert.equal(cadenceDelaySeconds(settings, () => 1), 95);
});

test("initial reports are staggered across the first cadence window", () => {
  const settings = { reportCadenceSeconds: 60, reportVarianceSeconds: 15 };
  assert.equal(initialCadenceDelaySeconds(settings, () => 0), 0);
  assert.equal(initialCadenceDelaySeconds(settings, () => 0.5), 37.5);
  assert.equal(initialCadenceDelaySeconds(settings, () => 1), 75);
});

test("cadence values remain safe and human readable", () => {
  assert.equal(cadenceDelaySeconds({ reportCadenceSeconds: 5, reportVarianceSeconds: 20 }, () => 0), 0.1);
  assert.equal(cadenceDelaySeconds({ reportCadenceSeconds: 5, reportVarianceSeconds: 20 }, () => 1), 9.9);
  assert.equal(formatCadenceSeconds(65), "1m 5s");
  assert.equal(formatCadenceSeconds(120), "2m");
  assert.equal(formatCadenceSeconds(9), "9s");
});
