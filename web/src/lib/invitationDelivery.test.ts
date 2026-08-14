import assert from "node:assert/strict";
import test from "node:test";
import { classifyInvitationEmailFailure, invitationDeliveryMessage } from "./invitationDelivery.ts";

test("reports missing provider configuration distinctly", () => {
  assert.equal(classifyInvitationEmailFailure("email_not_configured"), "not_configured");
  assert.match(invitationDeliveryMessage("not_configured") ?? "", /not been configured/i);
});

test("reports rate limiting without invalidating the invitation", () => {
  assert.equal(classifyInvitationEmailFailure("email_rate_limit"), "rate_limited");
  assert.match(invitationDeliveryMessage("rate_limited") ?? "", /wait an hour/i);
});

test("uses a safe generic message for provider and unknown failures", () => {
  assert.equal(classifyInvitationEmailFailure("email_delivery_failed"), "failed");
  assert.equal(classifyInvitationEmailFailure(undefined), "failed");
  assert.match(invitationDeliveryMessage("failed") ?? "", /still valid/i);
});
