import { createServerClient } from "@supabase/ssr";
import { NextResponse, type NextRequest } from "next/server";

export async function GET(request: NextRequest) {
  const requestUrl = new URL(request.url);
  const code = requestUrl.searchParams.get("code");
  const requestedPath = requestUrl.searchParams.get("next") ?? "/";
  const nextPath = requestedPath.startsWith("/") && !requestedPath.startsWith("//")
    ? requestedPath
    : "/";
  const response = NextResponse.redirect(new URL(nextPath, requestUrl.origin));

  if (!code) {
    // The default Supabase email template can return an implicit-flow session
    // in the URL fragment. Fragments are browser-only, so send the browser to
    // the public bootstrap route and let the Supabase client persist it.
    return NextResponse.redirect(new URL("/?auth_callback=implicit", requestUrl.origin));
  }

  const supabaseUrl = process.env.NEXT_PUBLIC_SUPABASE_URL;
  const supabasePublishableKey = process.env.NEXT_PUBLIC_SUPABASE_PUBLISHABLE_KEY;
  if (!supabaseUrl || !supabasePublishableKey) {
    return NextResponse.redirect(new URL("/login?error=configuration", requestUrl.origin));
  }

  const supabase = createServerClient(supabaseUrl, supabasePublishableKey, {
    cookies: {
      getAll() {
        return request.cookies.getAll();
      },
      setAll(cookiesToSet, headers) {
        cookiesToSet.forEach(({ name, value, options }) => {
          response.cookies.set(name, value, options);
        });
        Object.entries(headers).forEach(([name, value]) => {
          response.headers.set(name, value);
        });
      },
    },
  });

  const { error } = await supabase.auth.exchangeCodeForSession(code);
  if (error) {
    console.error("[auth/callback] OAuth code exchange failed", {
      errorCode: error.code,
      message: error.message,
      status: error.status,
    });
    return NextResponse.redirect(new URL("/login?error=oauth_callback", requestUrl.origin));
  }

  console.info("[auth/callback] OAuth code exchange succeeded");
  return response;
}
