import { connection } from "next/server";
import { redirect } from "next/navigation";
import { AuthBootstrap } from "@/components/AuthBootstrap";
import { Dashboard } from "@/components/Dashboard";
import { getFamilyContext } from "@/lib/familyContext";
import { getLiveTelemetrySnapshot } from "@/lib/liveTelemetry";
import { createClient } from "@/lib/supabase/server";

export default async function Home() {
  await connection();
  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();

  const claims = identity?.claims;
  const userId = typeof claims?.sub === "string" ? claims.sub : null;
  if (!userId || !claims) return <AuthBootstrap />;

  const familyContext = await getFamilyContext(userId);
  if (!familyContext.activeFamily && !familyContext.error) redirect("/onboarding");

  const snapshot = familyContext.activeFamily
    ? await getLiveTelemetrySnapshot(familyContext.activeFamily.householdId)
    : { devices: [], householdId: null, accessVersion: null, error: familyContext.error };
  const userEmail = typeof claims.email === "string" ? claims.email : null;

  return (
    <Dashboard
      householdId={snapshot.householdId}
      householdAccessVersion={snapshot.accessVersion}
      initialLiveDevices={snapshot.devices}
      liveTelemetryError={snapshot.error}
      userEmail={userEmail}
      familyName={familyContext.activeFamily?.name ?? null}
      familyRole={familyContext.activeFamily?.role ?? null}
    />
  );
}
