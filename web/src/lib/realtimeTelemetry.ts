import { createClient } from "@/lib/supabase/client";
import { isPositionRow, positionToTelemetryDevice, type PositionRow } from "@/lib/telemetryRows";
import type { TelemetryDevice, TelemetrySource, TrailPoint } from "@/types/telemetry";

const INITIAL_FALLBACK_DELAY_MS = 30_000;
const MAX_FALLBACK_DELAY_MS = 120_000;
const POSITION_COLUMNS = "position_id,device_uid,household_id,message_id,latitude,longitude,battery,source,recorded_at,received_at,schema_version";

export function createRealtimeTelemetrySource(
  householdId: string,
  initialDevices: TelemetryDevice[],
): TelemetrySource {
  return {
    subscribe(listener, statusListener) {
      const supabase = createClient();
      const devices = new Map(initialDevices.map((device) => [device.id, device]));
      let active = true;
      let fallbackDelay = INITIAL_FALLBACK_DELAY_MS;
      let fallbackTimer: number | null = null;
      let realtimeConnected = false;
      let channel: ReturnType<typeof supabase.channel> | null = null;

      const publish = () => {
        listener([...devices.values()].sort((left, right) => left.id - right.id));
      };

      const updateFromRow = (value: unknown) => {
        if (!isPositionRow(value) || value.household_id !== householdId) return;
        const next = positionToTelemetryDevice(value);
        const current = devices.get(next.id);
        if (current && !isNewer(next, current)) return;
        devices.set(next.id, next);
        publish();
      };

      const refresh = async () => {
        const result = await supabase
          .from("device_latest_positions")
          .select(POSITION_COLUMNS)
          .eq("household_id", householdId);

        if (!active) return;
        if (result.error) {
          statusListener?.("degraded", result.error.message);
          return;
        }

        const incoming: PositionRow[] = Array.isArray(result.data)
          ? (result.data as unknown[]).filter((row): row is PositionRow => isPositionRow(row))
          : [];
        const incomingIds = new Set(incoming.map((row: PositionRow) => row.device_uid));
        devices.forEach((_device, deviceId) => {
          if (!incomingIds.has(deviceId)) devices.delete(deviceId);
        });
        incoming.forEach((row: PositionRow) => devices.set(row.device_uid, positionToTelemetryDevice(row)));
        publish();
      };

      const clearFallback = () => {
        if (fallbackTimer !== null) window.clearTimeout(fallbackTimer);
        fallbackTimer = null;
      };

      const scheduleFallback = () => {
        if (!active || realtimeConnected || fallbackTimer !== null) return;
        fallbackTimer = window.setTimeout(async () => {
          fallbackTimer = null;
          await refresh();
          fallbackDelay = Math.min(fallbackDelay * 2, MAX_FALLBACK_DELAY_MS);
          scheduleFallback();
        }, fallbackDelay);
      };

      const handleBroadcast = (payload: unknown) => {
        const row = extractBroadcastRecord(payload);
        updateFromRow(row);
      };

      publish();
      statusListener?.("connecting");

      const connect = async () => {
        await supabase.realtime.setAuth();
        if (!active) return;

        channel = supabase
          .channel(`household:${householdId}`, { config: { private: true } })
          .on("broadcast", { event: "INSERT" }, handleBroadcast)
          .on("broadcast", { event: "UPDATE" }, handleBroadcast)
          .subscribe((status: "SUBSCRIBED" | "TIMED_OUT" | "CLOSED" | "CHANNEL_ERROR", error?: Error) => {
            if (!active) return;
            if (status === "SUBSCRIBED") {
              realtimeConnected = true;
              fallbackDelay = INITIAL_FALLBACK_DELAY_MS;
              clearFallback();
              statusListener?.("connected");
              void refresh();
              return;
            }

            if (status === "CHANNEL_ERROR" || status === "TIMED_OUT" || status === "CLOSED") {
              realtimeConnected = false;
              statusListener?.("degraded", error?.message ?? "Realtime connection interrupted");
              scheduleFallback();
            }
          });
      };

      void connect().catch((error: unknown) => {
        if (!active) return;
        const message = error instanceof Error ? error.message : "Realtime authentication failed";
        statusListener?.("degraded", message);
        scheduleFallback();
      });

      const recoverSnapshot = () => {
        if (document.visibilityState === "visible") void refresh();
      };
      document.addEventListener("visibilitychange", recoverSnapshot);
      window.addEventListener("online", recoverSnapshot);

      return () => {
        active = false;
        realtimeConnected = false;
        clearFallback();
        document.removeEventListener("visibilitychange", recoverSnapshot);
        window.removeEventListener("online", recoverSnapshot);
        if (channel) void supabase.removeChannel(channel);
      };
    },
  };
}

export async function loadDeviceTrail(deviceId: number): Promise<TrailPoint[]> {
  const supabase = createClient();
  const since = new Date(Date.now() - 7 * 24 * 60 * 60 * 1000).toISOString();
  const { data, error } = await supabase
    .from("positions")
    .select("latitude,longitude,recorded_at,message_id")
    .eq("device_uid", deviceId)
    .gte("recorded_at", since)
    .order("recorded_at", { ascending: false })
    .order("message_id", { ascending: false })
    .limit(1000);

  if (error) throw error;

  return (data ?? [])
    .filter((row: unknown): row is Pick<PositionRow, "latitude" | "longitude" | "recorded_at" | "message_id"> => isTrailRow(row))
    .reverse()
    .map((row: Pick<PositionRow, "latitude" | "longitude" | "recorded_at">) => ({ lat: row.latitude, lon: row.longitude, recordedAt: row.recorded_at }));
}

function extractBroadcastRecord(value: unknown): unknown {
  if (!value || typeof value !== "object") return null;
  const envelope = value as Record<string, unknown>;
  const payload = envelope.payload;
  if (!payload || typeof payload !== "object") return null;
  const change = payload as Record<string, unknown>;
  return change.record ?? change.new ?? change.new_record ?? null;
}

function isNewer(next: TelemetryDevice, current: TelemetryDevice) {
  return next.lastUpdate > current.lastUpdate || (
    next.lastUpdate === current.lastUpdate && next.seq > current.seq
  );
}

function isTrailRow(value: unknown): value is Pick<PositionRow, "latitude" | "longitude" | "recorded_at" | "message_id"> {
  if (!value || typeof value !== "object") return false;
  const row = value as Record<string, unknown>;
  return (
    typeof row.latitude === "number" &&
    typeof row.longitude === "number" &&
    typeof row.recorded_at === "string" &&
    typeof row.message_id === "number"
  );
}
