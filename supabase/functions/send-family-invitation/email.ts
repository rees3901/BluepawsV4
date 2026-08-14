export const INVITATION_TOKEN_PATTERN = /^[0-9a-f]{64}$/;
export const INVITATION_ID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;

export interface InvitationEmailContent {
  subject: string;
  text: string;
  html: string;
}

export async function tokenMatchesHash(token: string, storedHash: string) {
  if (!INVITATION_TOKEN_PATTERN.test(token)) return false;
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(token));
  const calculatedHash = [...new Uint8Array(digest)]
    .map((value) => value.toString(16).padStart(2, "0"))
    .join("");
  const normalizedStoredHash = (storedHash.startsWith("\\x") ? storedHash.slice(2) : storedHash).toLowerCase();
  return calculatedHash === normalizedStoredHash;
}

export function invitationUrl(siteUrl: string, token: string) {
  if (!INVITATION_TOKEN_PATTERN.test(token)) throw new Error("Invalid invitation token");
  const url = new URL("/join", siteUrl);
  url.searchParams.set("token", token);
  return url.toString();
}

export function invitationEmail(familyName: string, joinUrl: string): InvitationEmailContent {
  const readableFamilyName = familyName.replace(/\s+/g, " ").trim();
  const safeFamilyName = escapeHtml(readableFamilyName);
  const safeJoinUrl = escapeHtml(joinUrl);
  return {
    subject: `You're invited to ${readableFamilyName} on Bluepaws`,
    text: `You have been invited to join ${readableFamilyName} on Bluepaws.\n\nAccept the invitation: ${joinUrl}\n\nThis private link expires after seven days and works only with the invited email address. If you were not expecting it, you can ignore this message.`,
    html: `<!doctype html><html><body style="margin:0;background:#0f1923;color:#e8edf2;font-family:Arial,sans-serif"><div style="max-width:560px;margin:0 auto;padding:40px 22px"><div style="color:#49aff5;font-size:13px;font-weight:700;letter-spacing:.08em;text-transform:uppercase">Bluepaws</div><h1 style="margin:18px 0 12px;font-size:26px">Join ${safeFamilyName}</h1><p style="color:#b5c2ce;line-height:1.6">You have been invited to share this Family's pets, live positions and trails on Bluepaws.</p><p style="margin:28px 0"><a href="${safeJoinUrl}" style="display:inline-block;padding:12px 18px;border-radius:8px;background:#1d9bf0;color:#fff;font-weight:700;text-decoration:none">Accept Family invitation</a></p><p style="color:#8899a6;font-size:13px;line-height:1.6">This private link expires after seven days and works only when you sign in with the invited email address. If you were not expecting this invitation, you can safely ignore it.</p></div></body></html>`,
  };
}

function escapeHtml(value: string) {
  return value.replace(/[&<>'"]/g, (character) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    "'": "&#39;",
    '"': "&quot;",
  })[character] ?? character);
}
