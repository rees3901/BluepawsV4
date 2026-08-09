import { mockTelemetrySource } from "@/data/mockTelemetry";
import type { TelemetrySource } from "@/types/telemetry";

/**
 * The public dashboard depends only on TelemetrySource. A later integration can
 * replace liveTelemetrySource with a Supabase Realtime adapter without changing
 * components. Tutorial and live telemetry are selected exclusively so simulated
 * devices can never be merged into customer data.
 * HTTPS ingestion belongs in a Supabase Edge Function, not in the browser.
 */
export function getTutorialTelemetrySource(): TelemetrySource {
  return {
    subscribe(listener, statusListener) {
      statusListener?.("connected");
      return mockTelemetrySource.subscribe(listener);
    },
  };
}
