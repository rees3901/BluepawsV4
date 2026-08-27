"use client";
import { useState } from "react";
import type { HubPresence } from "@/lib/hubPresence";
import { hubAvatar } from "@/lib/hubPresence";
import { createClient } from "@/lib/supabase/client";
import { DeviceCard, type DeviceCardProps } from "@/components/DeviceCard";

export function HubCard({ hub, onSaved, cardProps }: { hub: HubPresence; onSaved: () => void; cardProps: DeviceCardProps }) {
  const [editing, setEditing] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const avatar = hubAvatar(hub);
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
  return <DeviceCard {...cardProps} avatar={avatar} onAvatarEdit={() => setEditing(!editing)}
    hubDetails={<>
      <span className="label">Hub ID</span><span className="value">{hub.gateway_guid16.toString(16).padStart(4, "0")}</span>
      <span className="label">GPS fix</span><span className="value">{hub.fix_at ? new Date(hub.fix_at).toLocaleString() : "Not acquired"}</span>
      <span className="label">Home beacon</span><span className="value">{hub.ble_advertising ? "Advertising" : "Off"}</span>
    </>}
    hubActions={
        <button type="button" className="btn-action" disabled={busy} aria-pressed={hub.desired_ble_enabled}
          title="Home beacon only operates while connected to primary Home Wi-Fi"
          onClick={() => void save({ desired_ble_enabled: !hub.desired_ble_enabled })}><svg aria-hidden="true" width="14" height="16" viewBox="0 0 16 20"><path d="M4 5l9 10-5 4V1l5 4L4 15" fill="none" stroke="currentColor" strokeWidth="2"/></svg> Bluetooth {hub.desired_ble_enabled ? "On" : "Off"}</button>}
    hubFooter={<>
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
    </>}
  />;
}
