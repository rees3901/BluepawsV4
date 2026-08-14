"use server";

import { cookies } from "next/headers";
import { redirect } from "next/navigation";
import { createClient } from "@/lib/supabase/server";

export interface AcceptInvitationState {
  error: string | null;
}

export async function acceptInvitationAction(
  _previousState: AcceptInvitationState,
  formData: FormData,
): Promise<AcceptInvitationState> {
  const cookieStore = await cookies();
  const token = cookieStore.get("bp_family_invite")?.value ?? "";
  const displayName = String(formData.get("displayName") ?? "").trim();
  if (!/^[0-9a-f]{64}$/.test(token)) return { error: "This invitation link is missing or invalid." };
  if (displayName.length > 80) return { error: "Your display name must be no more than 80 characters." };

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/join");

  const { error } = await supabase.rpc("bluepaws_accept_family_invitation", {
    invitation_token: token,
    profile_display_name: displayName || null,
  });
  if (error) {
    console.error("Unable to accept Family invitation", { code: error.code, message: error.message });
    return { error: "This invitation is invalid, expired, revoked, or belongs to a different email address." };
  }

  cookieStore.delete("bp_family_invite");
  redirect("/");
}
