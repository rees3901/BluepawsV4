"use client";
import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import type { HubPresence } from "@/lib/hubPresence";
import { createClient } from "@/lib/supabase/client";
import { DeviceCard, type DeviceCardProps } from "@/components/DeviceCard";
import { DeviceReportModal } from "@/components/DeviceReportModal";
import { buildHubReport, hubReportCsv } from "@/lib/hubReports";
import { hubControlFeedback, type HubControlAttempt } from "@/lib/hubControlFeedback";
import { HUB_REPORTING, hubContactGrace, type HubReportingProfile } from "@/lib/hubReporting";

export function HubCard({ hub, onSaved, cardProps }: { hub: HubPresence; onSaved: () => void; cardProps: DeviceCardProps }) {
  const [busy, setBusy] = useState(false);
  const [commandOpen, setCommandOpen] = useState(false);
  const commandForm = useRef<HTMLFormElement>(null);
  useEffect(() => {
    if (!commandOpen) return;
    const previous = document.activeElement as HTMLElement | null;
    commandForm.current?.querySelector("select")?.focus();
    return () => previous?.focus();
  }, [commandOpen]);
  const [profile, setProfile] = useState<HubReportingProfile>(hub.reporting_profile ?? "normal");
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
  const offline = cardProps.ageSeconds >= hubContactGrace(hub.reporting_profile);
  // Reuse the dashboard's ticking contact age; keep render deterministic.
  const now = Date.parse(hub.received_at) + cardProps.ageSeconds * 1000;
  const feedback = hubControlFeedback(hub, attempt, now);
  const pending = busy || feedback?.state === "pending";
  const save = async (target: { enabled: boolean } | { profile: HubReportingProfile }) => {
    setBusy(true); setError(""); setAttempt(null);
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 10000);
    try {
      const values = "enabled" in target ? { desired_ble_enabled: target.enabled } : { desired_reporting_profile: target.profile };
      const { data, error: failure } = await createClient().from("hub_presence").update(values)
        .eq("gateway_guid16", hub.gateway_guid16).eq("household_id", hub.household_id)
        .select("settings_revision").abortSignal(controller.signal);
      if (failure || !data?.length) throw new Error("Hub settings could not be saved");
      setAttempt({ ...target, revision: data[0].settings_revision, startedAt: Date.now() });
      setCommandOpen(false);
      onSaved();
    } catch { setError("Could not confirm saving the change. Check your connection and refresh the hub status before retrying."); onSaved(); }
    finally { clearTimeout(timeout); setBusy(false); }
  };
  return <><DeviceCard {...cardProps} onReportLog={() => void loadReport()} onReportExport={() => void loadReport(true)}
    bluetoothEnabled={hub.ble_enabled} bluetoothToggleDisabled={pending || offline}
    onBluetoothToggle={() => void save({enabled: !hub.ble_enabled})}
    hubDetails={<>
      <span className="label">Hub ID</span><span className="value">{hub.gateway_guid16.toString(16).padStart(4, "0")}</span>
      <span className="label">GPS fix</span><span className="value">{hub.fix_at ? new Date(hub.fix_at).toLocaleString() : "Not acquired"}</span>
      <span className="label">Home beacon</span><span className="value">{hub.ble_advertising ? "Advertising" : "Off"}</span>
      <span className="label">Reporting profile</span><span className="value">{HUB_REPORTING[hub.reporting_profile ?? "normal"].label}</span>
    </>}
    hubActions={<>
        <button type="button" className="btn-action" disabled={pending || offline} aria-pressed={hub.ble_enabled}
          title="Home beacon only operates while connected to primary Home Wi-Fi"
          onClick={() => void save({enabled: !hub.ble_enabled})}><svg aria-hidden="true" width="14" height="16" viewBox="0 0 16 20"><path d="M4 5l9 10-5 4V1l5 4L4 15" fill="none" stroke="currentColor" strokeWidth="2"/></svg> Bluetooth {hub.ble_enabled ? "On" : "Off"}</button>
        <button type="button" className="btn-action btn-cmd" disabled={pending || offline}
          onClick={() => { setProfile(hub.reporting_profile ?? "normal"); setCommandOpen(true); }}>⌘ Cmd</button>
      </>}
    hubFooter={<>
      {offline && <p className="hub-control-feedback" role="status">Hub contact lost — last Wi-Fi signal is no longer current. Check hub power and Wi-Fi.</p>}
      {feedback && <p className={`hub-control-feedback ${feedback.state}`} role={feedback.state === "failed" ? "alert" : "status"}>{feedback.text}</p>}
      {!attempt && hub.desired_ble_enabled !== hub.ble_enabled && <p className="hub-control-feedback" role="status">Saved Bluetooth setting not yet confirmed by hub.</p>}
      {error && <p role="alert">{error}</p>}
    </>}
  />{commandOpen && typeof document !== "undefined" && createPortal(
    <div className="modal" role="dialog" aria-modal="true" aria-labelledby="hub-command-title" onKeyDown={e => {
      if (e.key === "Escape") { e.preventDefault(); setCommandOpen(false); }
      if (e.key !== "Tab") return;
      const items = commandForm.current?.querySelectorAll<HTMLElement>("select,button:not(:disabled)");
      if (!items?.length) return;
      if (e.shiftKey && document.activeElement === items[0]) { e.preventDefault(); items[items.length-1].focus(); }
      else if (!e.shiftKey && document.activeElement === items[items.length-1]) { e.preventDefault(); items[0].focus(); }
    }}>
      <form ref={commandForm} className="modal-content" onSubmit={e => { e.preventDefault(); void save({profile}); }}>
        <h2 id="hub-command-title">{hub.display_name} — Change Power Profile</h2>
        <div className="form-group"><label htmlFor="hub-reporting-profile">Reporting profile</label>
        <select id="hub-reporting-profile" value={profile} onChange={e => setProfile(e.target.value as HubReportingProfile)}>
          {(Object.keys(HUB_REPORTING) as HubReportingProfile[]).map(key =>
            <option key={key} value={key}>{HUB_REPORTING[key].label} — every {HUB_REPORTING[key].seconds} seconds</option>)}
        </select></div>
        <p>Changes this hub’s own reports only. LoRa reception and command handling remain always on.</p>
        {!hub.control_poll_s && <p role="status">This hub has not reported support for reporting profiles yet. Update its firmware first.</p>}
        {error && <p role="alert">{error}</p>}
        <div className="modal-actions">
          <button className="btn-primary" disabled={pending || offline || !hub.control_poll_s}>Apply profile</button>
          <button className="btn-secondary" type="button" onClick={() => setCommandOpen(false)}>Close</button>
        </div>
      </form>
    </div>, document.body)}
  {reportOpen && <DeviceReportModal deviceName={hub.display_name} entityLabel="Home Hub"
    reports={report ? [buildHubReport(report)] : []} loading={reportLoading} error={reportError}
    onClose={() => setReportOpen(false)} onDownload={() => { if (report) download(report); }} />}</>;
}
