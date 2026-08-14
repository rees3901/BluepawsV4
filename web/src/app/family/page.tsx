import Link from "next/link";
import { redirect } from "next/navigation";
import { getFamilyContext } from "@/lib/familyContext";
import { loadFamilySettings } from "@/lib/familySettings";
import { createClient } from "@/lib/supabase/server";
import { FamilySettingsClient } from "./FamilySettingsClient";

interface FamilyPageProps {
  searchParams: Promise<{ error?: string }>;
}

export default async function FamilyPage({ searchParams }: FamilyPageProps) {
  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();
  const userId = typeof identity?.claims?.sub === "string" ? identity.claims.sub : null;
  if (!userId) redirect("/login?next=/family");

  const context = await getFamilyContext(userId);
  if (!context.activeFamily && !context.error) redirect("/onboarding");
  if (!context.activeFamily) throw new Error(context.error ?? "Family unavailable");

  const settings = await loadFamilySettings(context.activeFamily.householdId, context.activeFamily.role);
  const switchError = (await searchParams).error === "switch";

  return (
    <main className="account-shell">
      <header className="account-header">
        <div><span className="login-brand">Bluepaws V4</span><h1>Family &amp; members</h1><p>Manage the people who share access to your pets.</p></div>
        <Link className="btn-secondary" href="/">Back to map</Link>
      </header>
      {switchError && <p className="settings-message error">That Family could not be selected.</p>}
      <FamilySettingsClient activeFamily={context.activeFamily} families={context.families} members={settings.members} invitations={settings.invitations} />
    </main>
  );
}
