export const SEARCH_SHARE_TOKEN_PATTERN = /^[0-9a-f]{64}$/;
export const SEARCH_SHARE_ID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[1-8][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;

export interface SearchPartyEmailContent {
  subject: string;
  text: string;
  html: string;
}

export async function tokenMatchesHash(token: string, storedHash: string) {
  if (!SEARCH_SHARE_TOKEN_PATTERN.test(token)) return false;
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(token));
  const calculatedHash = [...new Uint8Array(digest)]
    .map((value) => value.toString(16).padStart(2, "0"))
    .join("");
  const normalizedStoredHash = (storedHash.startsWith("\\x") ? storedHash.slice(2) : storedHash).toLowerCase();
  return calculatedHash === normalizedStoredHash;
}

export function searchPartyUrl(siteUrl: string, token: string) {
  if (!SEARCH_SHARE_TOKEN_PATTERN.test(token)) throw new Error("Invalid search-party token");
  return new URL(`/search/${token}`, siteUrl).toString();
}

export function searchPartyEmail(familyName: string, searchUrl: string, expiresAt: string): SearchPartyEmailContent {
  const readableFamilyName = familyName.replace(/\s+/g, " ").trim();
  const safeFamilyName = escapeHtml(readableFamilyName);
  const safeSearchUrl = escapeHtml(searchUrl);
  const expiry = formatExpiry(expiresAt);
  const safeExpiry = escapeHtml(expiry);

  return {
    subject: `${readableFamilyName} shared a temporary Bluepaws search map`,
    text: `${readableFamilyName} has shared a temporary Bluepaws search-party map with you.\n\nOpen the search map: ${searchUrl}\n\nThis read-only link expires ${expiry}. It cannot send collar commands or change account settings. If you were not expecting this link, you can ignore this message.`,
    html: `<!doctype html><html><body style="margin:0;background:#0f1923;color:#e8edf2;font-family:Arial,sans-serif"><div style="max-width:560px;margin:0 auto;padding:40px 22px"><div style="color:#49aff5;font-size:13px;font-weight:700;letter-spacing:.08em;text-transform:uppercase">🐾 Bluepaws search party</div><h1 style="margin:18px 0 12px;font-size:26px">Help search with ${safeFamilyName}</h1><p style="color:#b5c2ce;line-height:1.6">${safeFamilyName} has shared a temporary, read-only Bluepaws map so you can help locate their pets.</p><p style="margin:28px 0"><a href="${safeSearchUrl}" style="display:inline-block;padding:12px 18px;border-radius:8px;background:#1d9bf0;color:#fff;font-weight:700;text-decoration:none">Open search-party map</a></p><p style="color:#8899a6;font-size:13px;line-height:1.6">This link expires ${safeExpiry}. It can show current pet positions, but it cannot send collar commands, change settings or access billing. If you were not expecting this link, you can safely ignore it.</p></div></body></html>`,
  };
}

function formatExpiry(expiresAt: string) {
  const timestamp = Date.parse(expiresAt);
  if (!Number.isFinite(timestamp)) return "soon";
  return new Intl.DateTimeFormat("en-GB", {
    dateStyle: "medium",
    timeStyle: "short",
    timeZone: "Europe/London",
  }).format(new Date(timestamp));
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
