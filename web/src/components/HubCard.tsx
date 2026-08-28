"use client";
import { useState } from "react";
import type { HubPresence } from "@/lib/hubPresence";
import { createClient } from "@/lib/supabase/client";
import { DeviceCard, type DeviceCardProps } from "@/components/DeviceCard";
import { DeviceReportModal } from "@/components/DeviceReportModal";
import { buildHubReport, hubReportCsv } from "@/lib/hubReports";
import { HUB_CONTACT_GRACE_SECONDS, hubControlFeedback, type HubControlAttempt } from "@/lib/hubControlFeedback";

export function HubCard({ hub, onSaved, cardProps }: { hub: HubPresence; onSaved: () => void; cardProps: DeviceCardProps }) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const [attempt, setAttempt] = useState<HubControlAttempt | null>(null);
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
  const offline = cardProps.ageSeconds >= HUB_CONTACT_GRACE_SECONDS;
  // Reuse the dashboard's ticking contact age; keep render deterministic.
  const now = Date.parse(hub.received_at) + cardProps.ageSeconds * 1000;
  const feedback = hubControlFeedback(hub, attempt, now);
  const pending = busy || feedback?.state === "pending";
  const save = async (enabled: boolean) => {
    setBusy(true); setError(""); setAttempt(null);
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 10000);
    try {
      const { data, error: failure } = await createClient().from("hub_presence").update({ desired_ble_enabled: enabled })
        .eq("gateway_guid16", hub.gateway_guid16).eq("household_id", hub.household_id)
        .select("settings_revision").abortSignal(controller.signal);
      if (failure || !data?.length) throw new Error("Hub settings could not be saved");
      setAttempt({ enabled, revision: data[0].settings_revision, startedAt: Date.now() });
      onSaved();
    } catch { setError("Could not confirm saving the change. Check your connection and refresh the hub status before retrying."); onSaved(); }
    finally { clearTimeout(timeout); setBusy(false); }
  };
  return <><DeviceCard {...cardProps} onReportLog={() => void loadReport()} onReportExport={() => void loadReport(true)}
    hubDetails={<>
      <span className="label">Hub ID</span><span className="value">{hub.gateway_guid16.toString(16).padStart(4, "0")}</span>
      <span className="label">GPS fix</span><span className="value">{hub.fix_at ? new Date(hub.fix_at).toLocaleString() : "Not acquired"}</span>
      <span className="label">Home beacon</span><span className="value">{hub.ble_advertising ? "Advertising" : "Off"}</span>
    </>}
    hubActions={
        <button type="button" className="btn-action" disabled={pending || offline} aria-pressed={hub.ble_enabled}
          title="Home beacon only operates while connected to primary Home Wi-Fi"
          onClick={() => void save(!hub.ble_enabled)}><svg aria-hidden="true" width="14" height="16" viewBox="0 0 16 20"><path d="M4 5l9 10-5 4V1l5 4L4 15" fill="none" stroke="currentColor" strokeWidth="2"/></svg> Bluetooth {hub.ble_enabled ? "On" : "Off"}</button>}
    hubFooter={<>
      {offline && <p className="hub-control-feedback" role="status">Hub contact lost — last Wi-Fi signal is no longer current. Check hub power and Wi-Fi.</p>}
      {feedback && <p className={`hub-control-feedback ${feedback.state}`} role={feedback.state === "failed" ? "alert" : "status"}>{feedback.text}</p>}
      {!attempt && hub.desired_ble_enabled !== hub.ble_enabled && <p className="hub-control-feedback" role="status">Saved Bluetooth setting not yet confirmed by hub.</p>}
      {error && <p role="alert">{error}</p>}
    </>}
  />{reportOpen && <DeviceReportModal deviceName={hub.display_name} entityLabel="Home Hub"
    reports={report ? [buildHubReport(report)] : []} loading={reportLoading} error={reportError}
    onClose={() => setReportOpen(false)} onDownload={() => { if (report) download(report); }} />}</>;
}
