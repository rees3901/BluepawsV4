import assert from "node:assert/strict";
import test from "node:test";
import { readFileSync } from "node:fs";
import { CUSTOMER_POWER_PROFILES, powerProfileLabel } from "./powerProfiles.ts";

test("profile queue requests one hour without changing LED command expiry", () => {
  const source = readFileSync(new URL("./deviceCommands.ts", import.meta.url), "utf8");
  const [profile, led] = source.split("export async function queueLedFindCommand");
  assert.match(profile, /requested_expires_in: "01:00:00"/);
  assert.match(led, /requested_expires_in: "00:10:00"/);
});

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
