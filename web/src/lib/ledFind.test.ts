import { strict as assert } from "node:assert";
import { test } from "node:test";
import { LED_INTERVALS, LED_DURATIONS, ledIntervalsForProfile, ledFindPayload, ledTimeLabel, supportsLedFind } from "./ledFind.ts";

test("fast LED cycles require the reported Emergency Lost profile", () => {
  for (const profile of [undefined, "Normal", "PowerSave", "Active", "Debug", "Lost", "unknown"]) {
    assert.deepEqual(ledIntervalsForProfile(profile), [60, 120, 300, 600]);
  }
  assert.deepEqual(ledIntervalsForProfile("Emergency Lost"), [10, 30, 60, 120, 300, 600]);
});

test("every offered LED interval and duration produces a valid payload", () => {
  for (const interval of LED_INTERVALS) for (const duration of LED_DURATIONS) {
    assert.deepEqual(ledFindPayload("repeat", duration, interval), { action: "repeat", duration_s: duration, interval_s: interval });
  }
  assert.deepEqual(ledFindPayload("repeat"), { action: "repeat", duration_s: 600, interval_s: 60 });
});
test("LED commands reject invalid bounds and non-integral values", () => {
  for (const duration of [9, 14401, 10.5, NaN, Infinity]) assert.throws(() => ledFindPayload("repeat", duration));
  for (const interval of [9, 601, 10.5, NaN, Infinity]) assert.throws(() => ledFindPayload("repeat", 600, interval));
});
test("one-shot and stop remain independent of repeat settings", () => {
  assert.deepEqual(ledFindPayload("flash", 14400, 10), { action: "flash" });
  assert.deepEqual(ledFindPayload("stop"), { action: "stop" });
  assert.equal(ledTimeLabel(60), "1 minute");
  assert.equal(ledTimeLabel(14400), "4 hours");
  assert.equal(supportsLedFind(3004), true);
  assert.equal(supportsLedFind(1002), false);
});
