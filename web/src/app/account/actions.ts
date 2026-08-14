"use server";

import { revalidatePath } from "next/cache";
import { redirect } from "next/navigation";
import { createClient } from "@/lib/supabase/server";

export interface ProfileActionState {
  error: string | null;
  success: string | null;
}

export async function updateProfileAction(
  _previousState: ProfileActionState,
  formData: FormData,
): Promise<ProfileActionState> {
  const displayName = String(formData.get("displayName") ?? "").trim();
  if (displayName.length < 1 || displayName.length > 80) {
    return { error: "Your display name must be between 1 and 80 characters.", success: null };
  }

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  const userId = typeof identity?.claims?.sub === "string" ? identity.claims.sub : null;
  if (!userId) redirect("/login?next=/account");

  const { error } = await supabase.from("profiles").update({ display_name: displayName }).eq("user_id", userId);
  if (error) {
    console.error("Unable to update Bluepaws profile", { code: error.code, message: error.message });
    return { error: "Your profile could not be saved. Please try again.", success: null };
  }

  revalidatePath("/");
  revalidatePath("/account");
  return { error: null, success: "Profile saved." };
}
