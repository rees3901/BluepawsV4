import { mockTelemetrySource } from "@/data/mockTelemetry";
import type { TelemetrySource } from "@/types/telemetry";

export type TelemetryMode = "live" | "tutorial";

const liveTelemetrySource: TelemetrySource = {
  subscribe() {
    // Intentionally idle until the Supabase Realtime adapter is connected.
    return () => undefined;
  },
};

/**
 * The public dashboard depends only on TelemetrySource. A later integration can
 * replace liveTelemetrySource with a Supabase Realtime adapter without changing
 * components. Tutorial and live telemetry are selected exclusively so simulated
 * devices can never be merged into customer data.
 * HTTPS ingestion belongs in a Supabase Edge Function, not in the browser.
 */
export function getTelemetrySource(mode: TelemetryMode): TelemetrySource {
  return mode === "tutorial" ? mockTelemetrySource : liveTelemetrySource;
}
