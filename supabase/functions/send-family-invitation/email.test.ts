import assert from "node:assert/strict";
import test from "node:test";
import { invitationEmail, invitationUrl, tokenMatchesHash } from "./email.ts";

const TOKEN = "a".repeat(64);
const TOKEN_HASH = "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb";

test("matches the raw invitation token to the stored SHA-256 hash", async () => {
  assert.equal(await tokenMatchesHash(TOKEN, `\\x${TOKEN_HASH}`), true);
  assert.equal(await tokenMatchesHash("b".repeat(64), `\\x${TOKEN_HASH}`), false);
});

test("builds the canonical join URL without accepting an arbitrary path", () => {
  assert.equal(
    invitationUrl("https://bluepaws.example/account", TOKEN),
    `https://bluepaws.example/join?token=${TOKEN}`,
  );
});

test("escapes Family names in HTML while retaining readable text", () => {
  const content = invitationEmail("Cats &\n<Friends>", `https://bluepaws.example/join?token=${TOKEN}`);
  assert.match(content.subject, /Cats & <Friends>/);
  assert.doesNotMatch(content.subject, /\n/);
  assert.match(content.text, /Cats & <Friends>/);
  assert.match(content.html, /Cats &amp; &lt;Friends&gt;/);
  assert.doesNotMatch(content.html, /Cats & <Friends>/);
});
