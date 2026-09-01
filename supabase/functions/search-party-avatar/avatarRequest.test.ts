import test from "node:test";
import assert from "node:assert/strict";
import { parseSearchPartyAvatarRequest } from "./avatarRequest.ts";

test("accepts a strict token-scoped collar or hub avatar request", () => {
  const token = "A".repeat(64);
  assert.deepEqual(parseSearchPartyAvatarRequest(new URL(`https://example.test/?token=${token}&entity=hub&id=16`)), {
    token: token.toLowerCase(), entity: "hub", id: 16,
  });
});

test("rejects malformed or out-of-range avatar requests", () => {
  assert.equal(parseSearchPartyAvatarRequest(new URL("https://example.test/?token=bad&entity=hub&id=16")), null);
  assert.equal(parseSearchPartyAvatarRequest(new URL(`https://example.test/?token=${"a".repeat(64)}&entity=owner&id=16`)), null);
  assert.equal(parseSearchPartyAvatarRequest(new URL(`https://example.test/?token=${"a".repeat(64)}&entity=collar&id=65536`)), null);
});
