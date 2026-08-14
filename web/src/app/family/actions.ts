"use server";

import { revalidatePath } from "next/cache";
import { redirect } from "next/navigation";
import { CANONICAL_SITE_URL } from "@/lib/authRedirect";
import { createClient } from "@/lib/supabase/server";

export interface InvitationActionState {
  error: string | null;
  invitationUrl: string | null;
  invitedEmail: string | null;
  expiresAt: string | null;
}

export async function createInvitationAction(
  _previousState: InvitationActionState,
  formData: FormData,
): Promise<InvitationActionState> {
  const householdId = String(formData.get("householdId") ?? "");
  const invitedEmail = String(formData.get("email") ?? "").trim().toLowerCase();
  if (!householdId || invitedEmail.length < 3 || invitedEmail.length > 320 || !invitedEmail.includes("@")) {
    return { error: "Enter a valid email address.", invitationUrl: null, invitedEmail: null, expiresAt: null };
  }

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/family");

  const { data, error } = await supabase.rpc("bluepaws_create_family_invitation", {
    requested_household_id: householdId,
    invited_email: invitedEmail,
  });

  if (error) {
    console.error("Unable to create Family invitation", { code: error.code, message: error.message });
    return { error: "The invitation could not be created. Check your Owner access and try again.", invitationUrl: null, invitedEmail: null, expiresAt: null };
  }

  const invitation = Array.isArray(data) ? data[0] : null;
  if (!invitation || typeof invitation.invitation_token !== "string") {
    return { error: "The invitation was created without a shareable link.", invitationUrl: null, invitedEmail: null, expiresAt: null };
  }

  revalidatePath("/family");
  return {
    error: null,
    invitationUrl: `${CANONICAL_SITE_URL}/join?token=${invitation.invitation_token}`,
    invitedEmail,
    expiresAt: typeof invitation.invitation_expires_at === "string" ? invitation.invitation_expires_at : null,
  };
}

export async function revokeInvitationAction(formData: FormData) {
  const invitationId = String(formData.get("invitationId") ?? "");
  if (!invitationId) return;

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/family");

  const { error } = await supabase.rpc("bluepaws_revoke_family_invitation", {
    requested_invitation_id: invitationId,
  });
  if (error) console.error("Unable to revoke Family invitation", { code: error.code, message: error.message });
  revalidatePath("/family");
}

export async function setActiveFamilyAction(formData: FormData) {
  const householdId = String(formData.get("householdId") ?? "");
  if (!householdId) return;

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/family");

  const { error } = await supabase.rpc("bluepaws_set_active_family", {
    requested_household_id: householdId,
  });
  if (error) {
    console.error("Unable to change active Family", { code: error.code, message: error.message });
    redirect("/family?error=switch");
  }

  redirect("/");
}
