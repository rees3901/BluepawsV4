import assert from "node:assert/strict";
import test from "node:test";
import { CUSTOMER_POWER_PROFILES, powerProfileLabel } from "./powerProfiles.ts";

test("customer profile choices use the canonical backend values", () => {
  assert.deepEqual(CUSTOMER_POWER_PROFILES, [
    { value: "normal", label: "Normal" },
    { value: "power_save", label: "Power Save" },
    { value: "active", label: "Active" },
    { value: "lost_alert", label: "Emergency Lost" },
  ]);
  assert.equal(powerProfileLabel("active"), "Active");
});

import { ledFindPayload, supportsLedFind } from "./ledFind.ts";
test("LED Find has bounded durations and distinct flash/repeat/stop actions", () => {
 assert.deepEqual(ledFindPayload("flash"),{action:"flash"});
 assert.deepEqual(ledFindPayload("stop"),{action:"stop"});
 assert.deepEqual(ledFindPayload("repeat"),{action:"repeat",duration_s:600,interval_s:60});
 for(const value of [0,14401,1.5,NaN]) assert.throws(()=>ledFindPayload("repeat",value));
 assert(supportsLedFind(3004)); assert(!supportsLedFind(1001)); assert(!supportsLedFind(3005));
});
