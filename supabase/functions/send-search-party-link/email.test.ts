import assert from "node:assert/strict";
import test from "node:test";
import { searchPartyEmail, searchPartyUrl, tokenMatchesHash } from "./email.ts";

const TOKEN = "a".repeat(64);
const TOKEN_HASH = "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb";

test("matches the raw search-party token to the stored SHA-256 hash", async () => {
  assert.equal(await tokenMatchesHash(TOKEN, `\\x${TOKEN_HASH}`), true);
  assert.equal(await tokenMatchesHash("b".repeat(64), `\\x${TOKEN_HASH}`), false);
});

test("builds the canonical search-party URL", () => {
  assert.equal(
    searchPartyUrl("https://bluepaws.example/account", TOKEN),
    `https://bluepaws.example/search/${TOKEN}`,
  );
});

test("escapes Family names in the search-party email", () => {
  const content = searchPartyEmail("Cats &\n<Friends>", `https://bluepaws.example/search/${TOKEN}`, "2026-08-20T19:14:15.000Z");
  assert.match(content.subject, /Cats & <Friends>/);
  assert.doesNotMatch(content.subject, /\n/);
  assert.match(content.text, /Cats & <Friends>/);
  assert.match(content.html, /Cats &amp; &lt;Friends&gt;/);
  assert.doesNotMatch(content.html, /Cats & <Friends>/);
  assert.match(content.html, /Open search-party map/);
});
