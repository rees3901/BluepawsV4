import assert from "node:assert/strict";
import test from "node:test";
import { resolveOwnedFamilyBillingAccounts } from "./familyBillingRows.ts";

test("returns only billing accounts owned by the signed-in person", () => {
  const accounts = resolveOwnedFamilyBillingAccounts(
    "mr-jones",
    [
      { household_id: "mr-family", billing_owner_user_id: "mr-jones" },
      { household_id: "mrs-family", billing_owner_user_id: "mrs-jones" },
    ],
    [
      { id: "mr-family", name: "Mr Jones Family" },
      { id: "mrs-family", name: "Mrs Jones Family" },
    ],
  );

  assert.deepEqual(accounts, [{ householdId: "mr-family", familyName: "Mr Jones Family" }]);
});

test("ignores malformed or unavailable Family records", () => {
  assert.deepEqual(resolveOwnedFamilyBillingAccounts(
    "mr-jones",
    [{ household_id: "missing-family", billing_owner_user_id: "mr-jones" }],
    [],
  ), []);
});
