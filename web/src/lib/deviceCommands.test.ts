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
