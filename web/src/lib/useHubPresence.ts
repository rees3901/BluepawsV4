"use client";
import { useCallback, useEffect, useState } from "react";
import { createClient } from "@/lib/supabase/client";
import type { HubPresence } from "./hubPresence";
export function useHubPresence(family: string | null) {
  const [state, setState] = useState<{ family: string | null; hubs: HubPresence[]; error: string | null }>({ family: null, hubs: [], error: null });
  const [version, setVersion] = useState(0);
  const refresh = useCallback(() => setVersion(v => v + 1), []);
  useEffect(() => {
    if (!family) return;
    let active = true, busy = false, lastLoad = 0, nextDelay = 10000;
    let controller: AbortController | null = null;
    const db = createClient();
    const load = async () => {
      if (busy || document.hidden) return;
      busy = true;
      const requestController = new AbortController();
      controller = requestController;
      const timeout = setTimeout(() => requestController.abort(), 10000);
      try {
        const { data, error } = await db.from("hub_presence").select("*").eq("household_id", family)
          .order("gateway_guid16").abortSignal(requestController.signal);
        if (error) throw error;
        const hubs = (data ?? []) as HubPresence[];
        nextDelay = hubs.some(h => h.settings_revision > h.applied_revision) ? 2000 : 10000;
        if (active) setState({ family, hubs, error: null });
      } catch {
        // Preserve last-known cards, but never carry data across Families.
        if (active) setState(previous => ({ family, hubs: previous.family === family ? previous.hubs : [], error: "Home Hub status could not be loaded" }));
        nextDelay = 10000;
      } finally { clearTimeout(timeout); controller = null; busy = false; lastLoad = Date.now(); }
    };
    void load();
    const timer = setInterval(() => { if (Date.now() - lastLoad >= nextDelay) void load(); }, 1000);
    document.addEventListener("visibilitychange", load);
    return () => { active = false; controller?.abort(); clearInterval(timer); document.removeEventListener("visibilitychange", load); };
  }, [family, version]);
  return { hubs: family && state.family === family ? state.hubs : [], error: family && state.family === family ? state.error : null, refresh };
}
