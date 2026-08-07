import type { TelemetrySource } from "@/types/telemetry";
import { mockTelemetrySource } from "@/data/mockTelemetry";

/**
 * The public dashboard depends only on TelemetrySource. A later integration can
 * replace this with a Supabase Realtime adapter without changing components.
 * HTTPS ingestion belongs in a Supabase Edge Function, not in the browser.
 */
export const telemetrySource: TelemetrySource = mockTelemetrySource;
