import "jsr:@supabase/functions-js/edge-runtime.d.ts";
import { createClient } from "@supabase/supabase-js";
import { parseSearchPartyAvatarRequest } from "./avatarRequest.ts";

const SIGNED_URL_LIFETIME_SECONDS = 60;

Deno.serve(async (request: Request) => {
  if (request.method !== "GET") return response("Method not allowed", 405);
  const avatarRequest = parseSearchPartyAvatarRequest(new URL(request.url));
  if (!avatarRequest) return response("Avatar unavailable", 404);

  const supabaseUrl = Deno.env.get("SUPABASE_URL");
  const serviceKey = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY");
  if (!supabaseUrl || !serviceKey) return response("Avatar service unavailable", 503);

  const supabase = createClient(supabaseUrl, serviceKey, {
    auth: { persistSession: false, autoRefreshToken: false },
  });
  const { data, error } = await supabase.rpc("bluepaws_resolve_search_party_avatar", {
    share_token: avatarRequest.token,
    requested_entity: avatarRequest.entity,
    requested_id: avatarRequest.id,
  });
  const avatar = Array.isArray(data) ? data[0] : null;
  if (error || !avatar || typeof avatar.bucket !== "string" || typeof avatar.object_path !== "string") {
    return response("Avatar unavailable", 404);
  }

  const signed = await supabase.storage.from(avatar.bucket).createSignedUrl(
    avatar.object_path,
    SIGNED_URL_LIFETIME_SECONDS,
  );
  if (signed.error || !signed.data?.signedUrl) return response("Avatar unavailable", 404);

  return new Response(null, {
    status: 302,
    headers: {
      Location: signed.data.signedUrl,
      "Cache-Control": `private, max-age=${SIGNED_URL_LIFETIME_SECONDS}`,
      "Referrer-Policy": "no-referrer",
      "X-Content-Type-Options": "nosniff",
    },
  });
});

function response(message: string, status: number) {
  return new Response(message, {
    status,
    headers: {
      "Content-Type": "text/plain; charset=utf-8",
      "Cache-Control": "no-store",
      "Referrer-Policy": "no-referrer",
      "X-Content-Type-Options": "nosniff",
    },
  });
}
