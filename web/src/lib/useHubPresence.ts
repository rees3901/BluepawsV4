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
    let active = true, busy = false;
    const db = createClient();
    const load = async () => {
      if (busy || document.hidden) return;
      busy = true;
      try {
        const { data, error } = await db.from("hub_presence").select("*").eq("household_id", family).order("gateway_guid16");
        if (active) setState({ family, hubs: error ? [] : (data ?? []) as HubPresence[], error: error ? "Home Hub status could not be loaded" : null });
      } catch {
        if (active) setState({ family, hubs: [], error: "Home Hub status could not be loaded" });
      } finally { busy = false; }
    };
    void load();
    const timer = setInterval(() => void load(), 10000);
    document.addEventListener("visibilitychange", load);
    return () => { active = false; clearInterval(timer); document.removeEventListener("visibilitychange", load); };
  }, [family, version]);
  return { hubs: family && state.family === family ? state.hubs : [], error: family && state.family === family ? state.error : null, refresh };
}
