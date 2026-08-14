import assert from "node:assert/strict";
import test from "node:test";
import { transportPresentation } from "./transportPath.ts";

test("labels direct cellular telemetry as 4G", () => {
  assert.deepEqual(transportPresentation("cellular_direct"), {
    badge: "4G",
    label: "Direct cellular link",
    cssClass: "transport-cellular",
  });
});

test("labels LoRa hub telemetry as RF", () => {
  assert.deepEqual(transportPresentation("lora_hub"), {
    badge: "RF",
    label: "LoRa hub radio link",
    cssClass: "transport-lora",
  });
});

test("does not guess a transport for legacy telemetry", () => {
  assert.equal(transportPresentation(null).badge, "—");
});
