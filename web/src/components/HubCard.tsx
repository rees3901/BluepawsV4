"use client";
import { useState } from "react";
import type { HubPresence } from "@/lib/hubPresence";
import { createClient } from "@/lib/supabase/client";
import { DeviceCard, type DeviceCardProps } from "@/components/DeviceCard";
import { DeviceReportModal } from "@/components/DeviceReportModal";
import { buildHubReport, hubReportCsv } from "@/lib/hubReports";

export function HubCard({ hub, onSaved, cardProps }: { hub: HubPresence; onSaved: () => void; cardProps: DeviceCardProps }) {
  const [editing, setEditing] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [reportOpen, setReportOpen] = useState(false);
  const [reportLoading, setReportLoading] = useState(false);
  const [reportError, setReportError] = useState<string | null>(null);
  const [report, setReport] = useState<HubPresence | null>(null);
  const download = (value: HubPresence) => {
    const url = URL.createObjectURL(new Blob([hubReportCsv(value)], {type:"text/csv;charset=utf-8"}));
    const a = document.createElement("a"); a.href=url; a.download=`bluepaws_hub_${value.gateway_guid16.toString(16).padStart(4,"0")}_report.csv`; a.click(); URL.revokeObjectURL(url);
  };
  const loadReport = async (exportOnly = false) => {
    setReportOpen(!exportOnly); setReportLoading(true); setReportError(null); setReport(null);
    try {
      const {data,error} = await createClient().from("hub_presence").select("*")
        .eq("gateway_guid16",hub.gateway_guid16).eq("household_id",hub.household_id).single();
      if (error || !data) throw new Error("Unable to load this hub’s latest report");
      setReport(data as HubPresence);
      if (exportOnly) download(data as HubPresence);
    } catch { setReportOpen(true); setReportError("Unable to load this hub’s latest report. Please try again."); }
    finally { setReportLoading(false); }
  };
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
  return <><DeviceCard {...cardProps} onReportLog={() => void loadReport()} onReportExport={() => void loadReport(true)}
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
      <button type="button" className="btn-action" onClick={() => setEditing(!editing)}>Rename hub</button>
      {pending && <p role="status">Settings pending — applied on the next hub check-in.</p>}
      {editing && <form className="hub-editor" onSubmit={event => {
        event.preventDefault(); const f = new FormData(event.currentTarget);
        void save({ display_name: String(f.get("name")).trim() });
      }}>
        <label>Name<input name="name" defaultValue={hub.display_name} maxLength={64} required /></label>
        <button disabled={busy} type="submit">Save name</button>
      </form>}
      {error && <p role="alert">{error}</p>}
    </>}
  />{reportOpen && <DeviceReportModal deviceName={hub.display_name} entityLabel="Home Hub"
    reports={report ? [buildHubReport(report)] : []} loading={reportLoading} error={reportError}
    onClose={() => setReportOpen(false)} onDownload={() => { if (report) download(report); }} />}</>;
}
