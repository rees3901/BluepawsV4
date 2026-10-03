import assert from "node:assert/strict";
import test from "node:test";
import { gnssQuality } from "./gnssQuality.ts";

test("GNSS accuracy bands use inclusive metre boundaries", () => {
  for (const [metres, level] of [[1,5],[5,5],[6,4],[10,4],[11,3],[25,3],[26,2],[50,2],[51,1]]) {
    assert.equal(gnssQuality(metres).level, level);
  }
});
test("missing, invalid and sentinel accuracy never imply a good fix", () => {
  for (const value of [undefined,null,0,-1,NaN,Infinity,65535]) assert.equal(gnssQuality(value).label, "Unknown");
  assert.equal(gnssQuality(5, false).label, "No fix");
});
