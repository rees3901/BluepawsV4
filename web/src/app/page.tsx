import { connection } from "next/server";
import { AuthBootstrap } from "@/components/AuthBootstrap";
import { Dashboard } from "@/components/Dashboard";
import { getLiveTelemetrySnapshot } from "@/lib/liveTelemetry";
import { createClient } from "@/lib/supabase/server";

export default async function Home() {
  await connection();
  const supabase = await createClient();
  const { data: identity } = await supabase.auth.getClaims();

  if (!identity?.claims?.sub) return <AuthBootstrap />;

  const snapshot = await getLiveTelemetrySnapshot();
  const userEmail = typeof identity.claims.email === "string" ? identity.claims.email : null;

  return (
    <Dashboard
      householdId={snapshot.householdId}
      initialLiveDevices={snapshot.devices}
      liveTelemetryError={snapshot.error}
      userEmail={userEmail}
    />
  );
}
