import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import { createClient } from "npm:@supabase/supabase-js@2.112.2";
import {
  SEARCH_SHARE_ID_PATTERN,
  SEARCH_SHARE_TOKEN_PATTERN,
  searchPartyEmail,
  searchPartyUrl,
  tokenMatchesHash,
} from "./email.ts";

const MAX_BODY_BYTES = 1_024;
const MAX_EMAILS_PER_FAMILY_PER_HOUR = 10;
const DEFAULT_SITE_URL = "https://bluepaws-v4-web.vercel.app";

interface RequestBody {
  shareId: string;
  shareToken: string;
}

interface SearchShareRow {
  id: string;
  household_id: string;
  helper_email: string;
  token_hash: string;
  expires_at: string;
  revoked_at: string | null;
}

Deno.serve(async (request: Request) => {
  const requestId = crypto.randomUUID();
  if (request.method !== "POST") return json({ error: "method_not_allowed", request_id: requestId }, 405, { Allow: "POST" });
  if (!request.headers.get("content-type")?.toLowerCase().startsWith("application/json")) {
    return json({ error: "content_type_must_be_json", request_id: requestId }, 415);
  }

  const declaredLength = Number(request.headers.get("content-length"));
  if (Number.isFinite(declaredLength) && declaredLength > MAX_BODY_BYTES) {
    return json({ error: "payload_too_large", request_id: requestId }, 413);
  }

  const authorization = request.headers.get("authorization");
  if (!authorization?.startsWith("Bearer ")) return json({ error: "unauthorized", request_id: requestId }, 401);

  let body: RequestBody;
  try {
    const rawBody = await request.text();
    if (new TextEncoder().encode(rawBody).byteLength > MAX_BODY_BYTES) {
      return json({ error: "payload_too_large", request_id: requestId }, 413);
    }
    const value = JSON.parse(rawBody) as Record<string, unknown>;
    if (
      Object.keys(value).some((key) => key !== "shareId" && key !== "shareToken")
      || typeof value.shareId !== "string"
      || !SEARCH_SHARE_ID_PATTERN.test(value.shareId)
      || typeof value.shareToken !== "string"
      || !SEARCH_SHARE_TOKEN_PATTERN.test(value.shareToken)
    ) return json({ error: "invalid_request", request_id: requestId }, 400);
    body = { shareId: value.shareId, shareToken: value.shareToken };
  } catch {
    return json({ error: "invalid_json", request_id: requestId }, 400);
  }

  try {
    const supabase = createClient(requiredEnvironment("SUPABASE_URL"), browserApiKey(), {
      global: { headers: { Authorization: authorization } },
      auth: { persistSession: false, autoRefreshToken: false },
    });
    const { data: authData, error: authError } = await supabase.auth.getUser();
    if (authError || !authData.user) return json({ error: "unauthorized", request_id: requestId }, 401);

    const { data: share, error: shareError } = await supabase
      .from("family_search_shares")
      .select("id,household_id,helper_email,token_hash,expires_at,revoked_at")
      .eq("id", body.shareId)
      .maybeSingle<SearchShareRow>();
    if (shareError) throw shareError;
    if (
      !share
      || share.revoked_at
      || Date.parse(share.expires_at) <= Date.now()
      || !(await tokenMatchesHash(body.shareToken, share.token_hash))
    ) return json({ error: "search_share_not_found", request_id: requestId }, 404);

    const hourAgo = new Date(Date.now() - 60 * 60 * 1_000).toISOString();
    const [{ data: household, error: householdError }, { count, error: countError }] = await Promise.all([
      supabase.from("households").select("name").eq("id", share.household_id).maybeSingle<{ name: string }>(),
      supabase.from("family_search_shares").select("id", { count: "exact", head: true }).eq("household_id", share.household_id).gte("created_at", hourAgo),
    ]);
    if (householdError || countError) throw householdError ?? countError;
    if (!household) return json({ error: "search_share_not_found", request_id: requestId }, 404);
    if ((count ?? 0) > MAX_EMAILS_PER_FAMILY_PER_HOUR) {
      return json({ error: "email_rate_limit", request_id: requestId }, 429);
    }

    const resendApiKey = Deno.env.get("RESEND_API_KEY");
    const emailFrom = Deno.env.get("BLUEPAWS_EMAIL_FROM");
    if (!resendApiKey || !emailFrom) {
      console.error("Search-party email is not configured", {
        requestId,
        missing: [!resendApiKey ? "RESEND_API_KEY" : null, !emailFrom ? "BLUEPAWS_EMAIL_FROM" : null].filter(Boolean),
      });
      return json({ error: "email_not_configured", request_id: requestId }, 503);
    }

    const linkUrl = searchPartyUrl(Deno.env.get("BLUEPAWS_SITE_URL") ?? DEFAULT_SITE_URL, body.shareToken);
    const content = searchPartyEmail(household.name, linkUrl, share.expires_at);
    const emailResponse = await fetch("https://api.resend.com/emails", {
      method: "POST",
      headers: {
        "authorization": `Bearer ${resendApiKey}`,
        "content-type": "application/json",
        "idempotency-key": `search-party-link/${share.id}`,
      },
      body: JSON.stringify({
        from: emailFrom,
        to: [share.helper_email],
        subject: content.subject,
        text: content.text,
        html: content.html,
      }),
    });
    if (!emailResponse.ok) {
      console.error("Search-party email provider rejected request", {
        requestId,
        providerRequestId: emailResponse.headers.get("x-request-id"),
        status: emailResponse.status,
      });
      return json({ error: "email_delivery_failed", request_id: requestId }, 502);
    }

    return json({ sent: true, request_id: requestId }, 200);
  } catch (error) {
    console.error("Search-party email failed", {
      requestId,
      message: error instanceof Error ? error.message : "unknown",
    });
    return json({ error: "service_unavailable", request_id: requestId }, 503);
  }
});

function requiredEnvironment(name: string) {
  const value = Deno.env.get(name);
  if (!value) throw new Error(`Missing ${name}`);
  return value;
}

function browserApiKey() {
  const legacyKey = Deno.env.get("SUPABASE_ANON_KEY");
  if (legacyKey) return legacyKey;
  const configuredKeys = Deno.env.get("SUPABASE_PUBLISHABLE_KEYS");
  if (configuredKeys) {
    try {
      const parsed = JSON.parse(configuredKeys);
      if (Array.isArray(parsed) && typeof parsed[0] === "string") return parsed[0];
      if (parsed && typeof parsed === "object") {
        for (const value of Object.values(parsed)) if (typeof value === "string") return value;
      }
    } catch {
      if (configuredKeys.startsWith("sb_publishable_")) return configuredKeys;
    }
  }
  throw new Error("Missing a Supabase browser API key");
}

function json(body: unknown, status: number, extraHeaders: HeadersInit = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: {
      "content-type": "application/json; charset=utf-8",
      "cache-control": "no-store",
      ...extraHeaders,
    },
  });
}
