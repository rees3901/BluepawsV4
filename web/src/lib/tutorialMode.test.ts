import assert from "node:assert/strict";
import test from "node:test";
import { resolveTutorialStartup } from "./tutorialMode.ts";

test("a first Family visit overrides old browser preferences and starts the tour", () => {
  assert.deepEqual(resolveTutorialStartup(true, false, true, true), {
    enabled: true,
    open: true,
    prompt: false,
  });
});

test("returning customers retain their saved Tutorial Mode preference", () => {
  assert.deepEqual(resolveTutorialStartup(false, false, false, false), {
    enabled: false,
    open: false,
    prompt: true,
  });
  assert.deepEqual(resolveTutorialStartup(false, true, true, true), {
    enabled: true,
    open: false,
    prompt: false,
  });
});
