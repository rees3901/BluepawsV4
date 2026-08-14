import assert from "node:assert/strict";
import test from "node:test";
import { getAuthCallbackUrl, sanitizeNextPath } from "./authRedirect.ts";

test("sanitizeNextPath allows local application paths", () => {
  assert.equal(sanitizeNextPath("/join"), "/join");
  assert.equal(sanitizeNextPath("/family?tab=people"), "/family?tab=people");
});

test("sanitizeNextPath rejects external and backslash redirects", () => {
  assert.equal(sanitizeNextPath("https://example.com"), "/");
  assert.equal(sanitizeNextPath("//example.com"), "/");
  assert.equal(sanitizeNextPath("/\\example.com"), "/");
});

test("auth callback carries a safe destination", () => {
  assert.equal(
    getAuthCallbackUrl("http://localhost:3000", "/join"),
    "http://localhost:3000/auth/callback?next=%2Fjoin",
  );
});
