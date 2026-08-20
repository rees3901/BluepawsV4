"use server";

import { revalidatePath } from "next/cache";
import { redirect } from "next/navigation";
import { CANONICAL_SITE_URL } from "@/lib/authRedirect";
import { normalizeFamilyName } from "@/lib/familyName";
import { classifyInvitationEmailFailure, type InvitationEmailDelivery } from "@/lib/invitationDelivery";
import { createClient } from "@/lib/supabase/server";

export interface InvitationActionState {
  error: string | null;
  invitationUrl: string | null;
  invitedEmail: string | null;
  expiresAt: string | null;
  emailDelivery: InvitationEmailDelivery;
}

export interface FamilyNameActionState {
  error: string | null;
  success: string | null;
}

export interface SearchPartyActionState {
  error: string | null;
  searchUrl: string | null;
  expiresAt: string | null;
}

export async function updateFamilyNameAction(
  _previousState: FamilyNameActionState,
  formData: FormData,
): Promise<FamilyNameActionState> {
  const householdId = String(formData.get("householdId") ?? "");
  const familyName = normalizeFamilyName(formData.get("familyName"));
  if (!householdId || !familyName) {
    return { error: "Your Family name must be between 1 and 80 characters.", success: null };
  }

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/account");

  const { data, error } = await supabase
    .from("households")
    .update({ name: familyName })
    .eq("id", householdId)
    .select("id");

  if (error || data?.length !== 1) {
    console.error("Unable to update Family name", { code: error?.code ?? "not_authorized", message: error?.message ?? "No Family updated" });
    return { error: "The Family name could not be updated. Check your Owner access and try again.", success: null };
  }

  revalidatePath("/account");
  revalidatePath("/");
  return { error: null, success: "Family name updated." };
}

export async function createInvitationAction(
  _previousState: InvitationActionState,
  formData: FormData,
): Promise<InvitationActionState> {
  const householdId = String(formData.get("householdId") ?? "");
  const invitedEmail = String(formData.get("email") ?? "").trim().toLowerCase();
  if (!householdId || invitedEmail.length < 3 || invitedEmail.length > 320 || !invitedEmail.includes("@")) {
    return { error: "Enter a valid email address.", invitationUrl: null, invitedEmail: null, expiresAt: null, emailDelivery: null };
  }

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/account");

  const { data, error } = await supabase.rpc("bluepaws_create_family_invitation", {
    requested_household_id: householdId,
    invited_email: invitedEmail,
  });

  if (error) {
    console.error("Unable to create Family invitation", { code: error.code, message: error.message });
    return { error: "The invitation could not be created. Check your Owner access and try again.", invitationUrl: null, invitedEmail: null, expiresAt: null, emailDelivery: null };
  }

  const invitation = Array.isArray(data) ? data[0] : null;
  if (!invitation || typeof invitation.invitation_token !== "string") {
    return { error: "The invitation was created without a shareable link.", invitationUrl: null, invitedEmail: null, expiresAt: null, emailDelivery: null };
  }

  const invitationId = typeof invitation.invitation_id === "string" ? invitation.invitation_id : null;
  let emailDelivery: InvitationEmailDelivery = "failed";
  if (invitationId) {
    const emailResult = await supabase.functions.invoke("send-family-invitation", {
      body: {
        invitationId,
        invitationToken: invitation.invitation_token,
      },
    });
    if (!emailResult.error && emailResult.data?.sent === true) {
      emailDelivery = "sent";
    } else {
      const providerCode = await readFunctionErrorCode(emailResult.error, emailResult.data);
      emailDelivery = classifyInvitationEmailFailure(providerCode);
      console.error("Family invitation was created but its email was not delivered", {
        errorName: emailResult.error?.name ?? "provider_response",
        message: emailResult.error?.message ?? "Email provider did not confirm delivery",
        providerCode,
      });
    }
  }

  revalidatePath("/account");
  return {
    error: null,
    invitationUrl: `${CANONICAL_SITE_URL}/join?token=${invitation.invitation_token}`,
    invitedEmail,
    expiresAt: typeof invitation.invitation_expires_at === "string" ? invitation.invitation_expires_at : null,
    emailDelivery,
  };
}

export async function createSearchPartyShareAction(
  _previousState: SearchPartyActionState,
  formData: FormData,
): Promise<SearchPartyActionState> {
  const householdId = String(formData.get("householdId") ?? "");
  if (!householdId) {
    return { error: "Choose a Family before creating a search-party link.", searchUrl: null, expiresAt: null };
  }

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/account");

  const { data, error } = await supabase.rpc("bluepaws_create_search_party_share", {
    requested_household_id: householdId,
  });

  if (error) {
    console.error("Unable to create search party link", { code: error.code, message: error.message });
    return { error: "The search-party link could not be created. Check your Owner access and try again.", searchUrl: null, expiresAt: null };
  }

  const share = Array.isArray(data) ? data[0] : null;
  if (!share || typeof share.share_token !== "string") {
    return { error: "The search-party link was created without a shareable token.", searchUrl: null, expiresAt: null };
  }

  revalidatePath("/account");
  return {
    error: null,
    searchUrl: `${CANONICAL_SITE_URL}/search/${share.share_token}`,
    expiresAt: typeof share.share_expires_at === "string" ? share.share_expires_at : null,
  };
}

export async function revokeInvitationAction(formData: FormData) {
  const invitationId = String(formData.get("invitationId") ?? "");
  if (!invitationId) return;

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/account");

  const { error } = await supabase.rpc("bluepaws_revoke_family_invitation", {
    requested_invitation_id: invitationId,
  });
  if (error) console.error("Unable to revoke Family invitation", { code: error.code, message: error.message });
  revalidatePath("/account");
}

export async function revokeSearchPartyShareAction(formData: FormData) {
  const shareId = String(formData.get("shareId") ?? "");
  if (!shareId) return;

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/account");

  const { error } = await supabase.rpc("bluepaws_revoke_search_party_share", {
    requested_share_id: shareId,
  });
  if (error) console.error("Unable to revoke search party link", { code: error.code, message: error.message });
  revalidatePath("/account");
}

export async function removeFamilyMemberAction(formData: FormData) {
  const householdId = String(formData.get("householdId") ?? "");
  const memberUserId = String(formData.get("memberUserId") ?? "");
  if (!householdId || !memberUserId) redirect("/account?error=remove#family");

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/account");

  const { error } = await supabase.rpc("bluepaws_remove_family_member", {
    requested_household_id: householdId,
    requested_user_id: memberUserId,
  });
  if (error) {
    console.error("Unable to remove Family member", { code: error.code, message: error.message });
    redirect("/account?error=remove#family");
  }

  revalidatePath("/account");
  redirect("/account?removed=1#family");
}

export async function setActiveFamilyAction(formData: FormData) {
  const householdId = String(formData.get("householdId") ?? "");
  if (!householdId) return;

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login?next=/account");

  const { error } = await supabase.rpc("bluepaws_set_active_family", {
    requested_household_id: householdId,
  });
  if (error) {
    console.error("Unable to change active Family", { code: error.code, message: error.message });
    redirect("/account?error=switch#family");
  }

  redirect("/");
}

async function readFunctionErrorCode(error: unknown, data: unknown) {
  if (data && typeof data === "object" && typeof (data as { error?: unknown }).error === "string") {
    return (data as { error: string }).error;
  }

  const context = error && typeof error === "object" ? (error as { context?: unknown }).context : null;
  if (!(context instanceof Response)) return null;
  try {
    const body = await context.clone().json() as { error?: unknown };
    return typeof body.error === "string" ? body.error : null;
  } catch {
    return null;
  }
}
