import assert from "node:assert/strict";
import test from "node:test";
import { defaultDeviceAvatar } from "./defaultDeviceAvatar.ts";

test("fallback avatars are anchored to the device id rather than card order", () => {
  const firstPosition = defaultDeviceAvatar(2001);
  const reorderedPosition = defaultDeviceAvatar(2001);

  assert.deepEqual(reorderedPosition, firstPosition);
});

test("nearby fleet devices receive deterministic fallback identities", () => {
  assert.notDeepEqual(defaultDeviceAvatar(2001), defaultDeviceAvatar(2002));
  assert.deepEqual(defaultDeviceAvatar(2002), defaultDeviceAvatar(2002));
});
