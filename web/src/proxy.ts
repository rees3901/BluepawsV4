import { NextResponse, type NextRequest } from "next/server";
import {
  CANONICAL_SITE_URL,
  isLegacyProductionHostname,
} from "@/lib/authRedirect";
import { updateSession } from "@/lib/supabase/proxy";

export async function proxy(request: NextRequest) {
  const requestHostname = (
    request.headers.get("x-forwarded-host") ??
    request.headers.get("host") ??
    request.nextUrl.hostname
  ).split(":")[0];

  if (isLegacyProductionHostname(requestHostname)) {
    const canonicalUrl = new URL(
      `${request.nextUrl.pathname}${request.nextUrl.search}`,
      CANONICAL_SITE_URL,
    );

    console.info("[auth] Redirecting legacy production hostname", {
      from: requestHostname,
      path: request.nextUrl.pathname,
    });
    return NextResponse.redirect(canonicalUrl, 308);
  }

  if (request.nextUrl.pathname === "/" && request.nextUrl.searchParams.has("code")) {
    const callbackUrl = request.nextUrl.clone();
    callbackUrl.pathname = "/auth/callback";

    console.info("[auth] Recovering OAuth code returned to the site root");
    return NextResponse.redirect(callbackUrl);
  }

  return updateSession(request);
}

export const config = {
  matcher: [
    "/((?!_next/static|_next/image|favicon.ico|.*\\.(?:svg|png|jpg|jpeg|gif|webp)$).*)",
  ],
};
