"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import { createClient } from "@/lib/supabase/client";
import { commandMessage, receiveDeadline, type CollarFeedback } from "@/lib/collarFeedback";
import { loadFaultReports } from "@/lib/collarFault";

export function useCollarFeedback(householdId: string | null, reportVersion: string) {
  const [state, setState] = useState<{ family: string | null; rows: Record<number, CollarFeedback> }>({ family: null, rows: {} });
  const refreshRef = useRef<() => void>(() => {});
  const refresh = useCallback(() => refreshRef.current(), []);

  useEffect(() => {
    if (!householdId) return;
    let active = true;
    let busy = false;
    let queued = false;
    let failures = 0;
    let timer: ReturnType<typeof setTimeout>;
    const supabase = createClient();
    const load = async () => {
      if (!active || document.hidden) return;
      if (busy) { queued = true; return; }
      clearTimeout(timer);
      busy = true;
      const started = Date.now();
      try {
        const { data, error } = await supabase.rpc("bluepaws_collar_feedback", { requested_household_id: householdId });
        if (!active) return;
        if (error || !Array.isArray(data)) throw error ?? new Error("Invalid feedback snapshot");
        failures = 0;
        const now = Date.now();
        setState(previous => {
          const rows: Record<number, CollarFeedback> = {};
          for (const row of data) {
            if (!Number.isInteger(row.device_id)) continue;
            const old = previous.family === householdId ? previous.rows[row.device_id] : undefined;
            rows[row.device_id] = {
              device_id: row.device_id, observation_id: row.observation_id,
              flags: Number.isInteger(row.flags) ? row.flags : null,
              faultReport: old?.observation_id === row.observation_id && old?.flags === row.flags ? old?.faultReport : null,
              command: row.command,
              rxWindowUntil: receiveDeadline(row.rx_window_remaining_ms, now, now - started,
                old?.observation_id === row.observation_id ? old?.rxWindowUntil : undefined),
            };
          }
          return { family: householdId, rows };
        });
        // Publish flags/ACK/receive-window state first. Optional diagnostic
        // enrichment must not delay these, and its network wait is bounded.
        const faultReports = await loadFaultReports(data, ids => supabase.from("observations")
          .select("id,device_guid16,flags,tx_reason,reset_reason:tlv_data->reset_reason")
          .eq("household_id", householdId).in("id", ids).abortSignal(AbortSignal.timeout(3000)));
        if (!active) return;
        if (Object.keys(faultReports).length) setState(previous => {
          if (previous.family !== householdId) return previous;
          const rows = { ...previous.rows };
          for (const row of data) {
            const current = rows[row.device_id];
            if (current?.observation_id === row.observation_id && current?.flags === row.flags && faultReports[row.device_id]) {
              rows[row.device_id] = { ...current, faultReport: faultReports[row.device_id] };
            }
          }
          return { ...previous, rows };
        });
        // Poll only while waiting for an ACK. Otherwise report broadcasts,
        // reconnect/focus and a successful command submission drive refreshes.
        if (data.some(row => commandMessage(row.command, now)?.pending)) timer = setTimeout(load, 5000);
      } catch {
        // Do not keep displaying an ACK/fault state after access is revoked.
        if (active) setState({ family: householdId, rows: {} });
        // Recover from a brief connection failure, but do not poll indefinitely
        // when the migration is absent or access has been removed.
        if (active && ++failures <= 3) timer = setTimeout(load, 1000 * 2 ** (failures - 1));
      } finally {
        busy = false;
        if (queued && active) { queued = false; void load(); }
      }
    };
    const refreshOnFocus = () => { void load(); };
    const onCommandChange = (event: Event) => {
      if ((event as CustomEvent).detail === householdId) void load();
    };
    refreshRef.current = refreshOnFocus;
    void load();
    document.addEventListener("visibilitychange", refreshOnFocus);
    window.addEventListener("online", refreshOnFocus);
    window.addEventListener("bluepaws:command-changed", onCommandChange);
    return () => {
      active = false;
      clearTimeout(timer);
      refreshRef.current = () => {};
      document.removeEventListener("visibilitychange", refreshOnFocus);
      window.removeEventListener("online", refreshOnFocus);
      window.removeEventListener("bluepaws:command-changed", onCommandChange);
    };
  }, [householdId]);

  useEffect(() => { refresh(); }, [reportVersion, refresh]);
  return { feedback: state.family === householdId ? state.rows : {}, refresh };
}
