import { connection } from "next/server";
import { Dashboard } from "@/components/Dashboard";
import { getLiveTelemetrySnapshot } from "@/lib/liveTelemetry";

export default async function Home() {
  await connection();
  const snapshot = await getLiveTelemetrySnapshot();

  return <Dashboard initialLiveDevices={snapshot.devices} liveTelemetryError={snapshot.error} />;
}
