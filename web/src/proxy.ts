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

  if (request.nextUrl.pathname === "/join" && request.nextUrl.searchParams.has("token")) {
    const token = request.nextUrl.searchParams.get("token")?.toLowerCase() ?? "";
    const cleanJoinUrl = request.nextUrl.clone();
    cleanJoinUrl.search = "";
    if (!/^[0-9a-f]{64}$/.test(token)) cleanJoinUrl.searchParams.set("error", "invalid");

    const response = NextResponse.redirect(cleanJoinUrl);
    if (/^[0-9a-f]{64}$/.test(token)) {
      response.cookies.set("bp_family_invite", token, {
        httpOnly: true,
        secure: request.nextUrl.protocol === "https:",
        sameSite: "lax",
        path: "/join",
        maxAge: 60 * 60 * 24 * 7,
      });
    }
    return response;
  }

  return updateSession(request);
}

export const config = {
  matcher: [
    "/((?!_next/static|_next/image|favicon.ico|.*\\.(?:svg|png|jpg|jpeg|gif|webp)$).*)",
  ],
};
