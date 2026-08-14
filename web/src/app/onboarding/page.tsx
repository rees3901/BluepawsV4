import { redirect } from "next/navigation";
import { getFamilyContext } from "@/lib/familyContext";
import { createClient } from "@/lib/supabase/server";
import { OnboardingForm } from "./OnboardingForm";

function displayNameFromClaims(claims: Record<string, unknown>) {
  const metadata = claims.user_metadata;
  if (metadata && typeof metadata === "object") {
    const values = metadata as Record<string, unknown>;
    const candidate = values.full_name ?? values.name;
    if (typeof candidate === "string" && candidate.trim()) return candidate.trim().slice(0, 80);
  }

  const email = claims.email;
  return typeof email === "string" ? email.split("@")[0].slice(0, 80) : "";
}

export default async function OnboardingPage() {
  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  const claims = identity?.claims;
  const userId = typeof claims?.sub === "string" ? claims.sub : null;

  if (!userId || !claims) redirect("/login");

  const familyContext = await getFamilyContext(userId);
  if (familyContext.activeFamily) redirect("/");

  return (
    <main className="login-shell onboarding-shell">
      <OnboardingForm defaultDisplayName={familyContext.displayName ?? displayNameFromClaims(claims)} />
    </main>
  );
}
