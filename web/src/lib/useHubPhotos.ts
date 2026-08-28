"use client";
import { useEffect, useState } from "react";
import { createClient } from "@/lib/supabase/client";
import { HUB_AVATAR_BUCKET } from "./hubAppearances";
import type { HubPresence } from "./hubPresence";

export function useHubPhotos(hubs: HubPresence[], family: string | null) {
  // Stable across heartbeat polls: download only when membership/path changes.
  const signature = JSON.stringify(hubs.filter(h => h.avatar_kind === "photo" && h.avatar_storage_path)
    .map(h => [h.gateway_guid16, h.avatar_storage_path]));
  const key = `${family}:${signature}`;
  const [state, setState] = useState<{key:string; urls:Record<number,string>}>({key:"",urls:{}});
  useEffect(() => {
    let active = true;
    const urls: Record<number,string> = {};
    const db = createClient();
    void Promise.all((JSON.parse(signature) as [number,string][]).map(async ([id,path]) => {
      try {
        const {data,error} = await db.storage.from(HUB_AVATAR_BUCKET).download(path);
        if (!active || error || !data) return;
        urls[id] = URL.createObjectURL(data);
      } catch { /* Emoji fallback; never make private photos public to bypass a failure. */ }
    })).then(() => { if (active) setState({key,urls}); });
    return () => { active = false; Object.values(urls).forEach(url => URL.revokeObjectURL(url)); };
  }, [signature,key]);
  return state.key === key ? state.urls : {};
}
