"use client";
import { useState } from "react";
import type { HubPresence } from "@/lib/hubPresence";
import { hubAvatar } from "@/lib/hubPresence";
import { createClient } from "@/lib/supabase/client";
import { googleMapsUrl } from "@/lib/mapLocation";

export function HubCard({ hub, now, onJump, onSaved }: { hub: HubPresence; now: number; onJump: () => void; onSaved: () => void }) {
  const [expanded, setExpanded] = useState(false);
  const [editing, setEditing] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const avatar = hubAvatar(hub);
  const age = Math.max(0, Math.floor((now - Date.parse(hub.received_at)) / 1000));
  const pending = hub.settings_revision > hub.applied_revision;
  const save = async (values: object) => {
    setBusy(true); setError("");
    try {
      const { data, error: failure } = await createClient().from("hub_presence").update(values)
        .eq("gateway_guid16", hub.gateway_guid16).eq("household_id", hub.household_id).select("gateway_guid16");
      if (failure || !data?.length) throw new Error("Hub settings could not be saved");
      setEditing(false); onSaved();
    } catch (e) { setError(e instanceof Error ? e.message : "Unable to save"); }
    finally { setBusy(false); }
  };
  return <article className={`device-card hub-card ${age >= 180 ? "stale" : ""}`}>
    <button className="hub-summary" type="button" onClick={() => setExpanded(!expanded)} aria-expanded={expanded}>
      <span className="card-avatar" style={{ borderColor: avatar.color }}>{avatar.emoji}</span>
      <span><strong>{hub.display_name}</strong><small>{hub.mode === "home" ? "Home" : hub.mode === "portable" ? "Portable" : "Off-Grid"} · Hub {hub.gateway_guid16.toString(16).padStart(4, "0")}</small>
      <small>{age >= 180 ? "Last heard" : "Online"} · {age}s ago · Wi-Fi {hub.wifi_rssi_dbm ?? "—"} dBm</small></span>
      <span>{expanded ? "▲" : "▼"}</span>
    </button>
    {expanded && <div className="card-detail">
      <p>{hub.latitude !== null && hub.longitude !== null
        ? <a href={googleMapsUrl(hub.latitude, hub.longitude)} target="_blank" rel="noopener noreferrer">{hub.latitude.toFixed(6)}, {hub.longitude.toFixed(6)}</a>
        : "Waiting for the hub’s own GPS fix"}</p>
      <p>{hub.fix_at ? "GPS fix: " + new Date(hub.fix_at).toLocaleString() : "No location inferred from collars."}</p>
      <p>Uptime {Math.floor(hub.uptime_s / 60)} min · Free memory {Math.round(hub.free_heap / 1024)} KB</p>
      <p>Home beacon: {hub.ble_advertising ? "advertising" : "off"}{hub.mode !== "home" ? " (disabled while roaming)" : ""}</p>
      <div className="card-actions">
        <button type="button" className="btn-action" disabled={hub.latitude === null} onClick={onJump}>↗ Jump To</button>
        <button type="button" className="btn-action" disabled={busy} aria-pressed={hub.desired_ble_enabled}
          title="Home beacon only operates while connected to primary Home Wi-Fi"
          onClick={() => void save({ desired_ble_enabled: !hub.desired_ble_enabled })}><svg aria-hidden="true" width="14" height="16" viewBox="0 0 16 20"><path d="M4 5l9 10-5 4V1l5 4L4 15" fill="none" stroke="currentColor" strokeWidth="2"/></svg> Bluetooth {hub.desired_ble_enabled ? "On" : "Off"}</button>
        <button type="button" className="btn-action" onClick={() => setEditing(!editing)}>Edit appearance</button>
      </div>
      {pending && <p role="status">Settings pending — applied on the next hub check-in.</p>}
      {editing && <form className="hub-editor" onSubmit={event => {
        event.preventDefault(); const f = new FormData(event.currentTarget);
        void save({ display_name: String(f.get("name")).trim(), home_emoji: f.get("home"), portable_emoji: f.get("portable"), marker_colour: f.get("colour") });
      }}>
        <label>Name<input name="name" defaultValue={hub.display_name} maxLength={64} required /></label>
        <label>Home emoji<input name="home" defaultValue={hub.home_emoji} maxLength={16} required /></label>
        <label>Portable / Off-Grid emoji<input name="portable" defaultValue={hub.portable_emoji} maxLength={16} required /></label>
        <label>Marker colour<input name="colour" type="color" defaultValue={hub.marker_colour} /></label>
        <button disabled={busy} type="submit">Save appearance</button>
      </form>}
      {error && <p role="alert">{error}</p>}
    </div>}
  </article>;
}
