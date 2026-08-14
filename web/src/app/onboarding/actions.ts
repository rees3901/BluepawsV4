"use server";

import { redirect } from "next/navigation";
import { createClient } from "@/lib/supabase/server";

export interface CreateFamilyState {
  error: string | null;
}

export async function createFamilyAction(
  _previousState: CreateFamilyState,
  formData: FormData,
): Promise<CreateFamilyState> {
  const displayName = String(formData.get("displayName") ?? "").trim();
  const familyName = String(formData.get("familyName") ?? "").trim();

  if (displayName.length < 1 || displayName.length > 80) {
    return { error: "Your display name must be between 1 and 80 characters." };
  }

  if (familyName.length < 1 || familyName.length > 80) {
    return { error: "Your Family name must be between 1 and 80 characters." };
  }

  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  if (!identity?.claims?.sub) redirect("/login");

  const { error } = await supabase.rpc("bluepaws_create_family", {
    family_name: familyName,
    profile_display_name: displayName,
  });

  if (error) {
    console.error("Unable to create Bluepaws Family", {
      code: error.code,
      message: error.message,
    });
    return { error: "Your Family could not be created. Please try again." };
  }

  redirect("/");
}
