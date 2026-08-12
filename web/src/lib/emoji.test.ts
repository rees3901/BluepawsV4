import assert from "node:assert/strict";
import test from "node:test";
import { emojiImageUrl, emojiToUnified } from "./emoji.ts";

test("converts a single emoji to its lowercase Unicode identifier", () => {
  assert.equal(emojiToUnified("🚗"), "1f697");
});

test("preserves joined emoji sequences for CDN rendering", () => {
  assert.equal(emojiToUnified("🐈‍⬛"), "1f408-200d-2b1b");
});

test("builds the Google emoji artwork URL", () => {
  assert.equal(
    emojiImageUrl("🐱"),
    "https://cdn.jsdelivr.net/npm/emoji-datasource-google/img/google/64/1f431.png",
  );
});
