const DEFAULT_CANONICAL_SITE_URL = "https://bluepaws-v4-web.vercel.app";

const LEGACY_PRODUCTION_HOSTNAMES = new Set([
  "bluepaws-v4-web-bluepaws-v4.vercel.app",
  "bluepaws-v4-web-git-main-bluepaws-v4.vercel.app",
]);

function withoutTrailingSlash(value: string) {
  return value.replace(/\/+$/, "");
}

export const CANONICAL_SITE_URL = withoutTrailingSlash(
  process.env.NEXT_PUBLIC_SITE_URL?.trim() || DEFAULT_CANONICAL_SITE_URL,
);

export const CANONICAL_HOSTNAME = new URL(CANONICAL_SITE_URL).hostname.toLowerCase();

export function isLegacyProductionHostname(hostname: string) {
  return LEGACY_PRODUCTION_HOSTNAMES.has(hostname.toLowerCase());
}

export function getAuthCallbackUrl(currentOrigin: string) {
  const currentHostname = new URL(currentOrigin).hostname.toLowerCase();
  const authOrigin = currentHostname === CANONICAL_HOSTNAME || isLegacyProductionHostname(currentHostname)
    ? CANONICAL_SITE_URL
    : withoutTrailingSlash(currentOrigin);

  return `${authOrigin}/auth/callback`;
}
