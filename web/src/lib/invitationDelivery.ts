export type InvitationEmailDelivery = "sent" | "not_configured" | "rate_limited" | "failed" | null;

export function classifyInvitationEmailFailure(providerCode: unknown): Exclude<InvitationEmailDelivery, "sent" | null> {
  if (providerCode === "email_not_configured") return "not_configured";
  if (providerCode === "email_rate_limit") return "rate_limited";
  return "failed";
}

export function invitationDeliveryMessage(delivery: InvitationEmailDelivery) {
  if (delivery === "not_configured") {
    return "Automatic email delivery has not been configured yet. The invitation is valid—share it using one of the options below.";
  }
  if (delivery === "rate_limited") {
    return "Too many invitations were requested for this Family. Wait an hour or share the valid invitation link below.";
  }
  if (delivery === "failed") {
    return "The email provider could not deliver this invitation. The invitation is still valid—share it using one of the options below.";
  }
  return null;
}
