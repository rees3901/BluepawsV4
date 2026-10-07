"use client";
import { useEffect, useRef, useState } from "react";
import { createPortal } from "react-dom";
import { HUB_BLE_POWERS, supportsHubBlePower, type HubBlePower } from "@/lib/hubBluetooth";

export function HubBluetoothDialog({ name, enabled, power, reportedPower, supported, steps, pending, offline, error, onSave, onClose }: {
  name: string; enabled: boolean; power: HubBlePower; reportedPower?: number | null; supported: boolean;
  pending: boolean; offline: boolean; error: string;
  steps?: number | null;
  onSave: (enabled: boolean, power?: HubBlePower) => void; onClose: () => void;
}) {
  const [advertising, setAdvertising] = useState(enabled);
  const [selectedPower, setSelectedPower] = useState<HubBlePower>(supportsHubBlePower(power, steps) ? power : 3);
  const form = useRef<HTMLFormElement>(null);
  useEffect(() => {
    const previous = document.activeElement as HTMLElement | null;
    form.current?.querySelector("input")?.focus();
    return () => previous?.focus();
  }, []);
  return createPortal(<div className="modal" role="dialog" aria-modal="true" aria-labelledby="hub-bluetooth-title" onKeyDown={event => {
    if (event.key === "Escape") { event.preventDefault(); onClose(); }
    if (event.key !== "Tab") return;
    const items = form.current?.querySelectorAll<HTMLElement>("input:not(:disabled),select:not(:disabled),button:not(:disabled)");
    if (!items?.length) return;
    if (event.shiftKey && document.activeElement === items[0]) { event.preventDefault(); items[items.length - 1].focus(); }
    else if (!event.shiftKey && document.activeElement === items[items.length - 1]) { event.preventDefault(); items[0].focus(); }
  }}><form ref={form} className="modal-content" onSubmit={event => { event.preventDefault(); onSave(advertising, supported && supportsHubBlePower(selectedPower, steps) ? selectedPower : undefined); }}>
    <h2 id="hub-bluetooth-title">{name} — Bluetooth</h2>
    <div className="form-group"><div className="toggle-row"><label htmlFor="hub-ble-advertising">Home beacon advertising</label>
      <label className="toggle-switch"><input id="hub-ble-advertising" type="checkbox" role="switch" checked={advertising} disabled={pending || offline} onChange={event => setAdvertising(event.target.checked)} /><span className="toggle-slider" /></label>
    </div></div>
    <div className="form-group"><label htmlFor="hub-ble-power">BLE advertising power</label>
      <select id="hub-ble-power" value={selectedPower} disabled={!supported || pending || offline} onChange={event => setSelectedPower(Number(event.target.value) as HubBlePower)}>
        {HUB_BLE_POWERS.map(power => <option key={power.value} value={power.value} disabled={!supportsHubBlePower(power.value, steps)}>{power.label}</option>)}
      </select></div>
    <p>Hub-reported advertising power: {supported ? `${reportedPower! > 0 ? "+" : ""}${reportedPower} dBm` : "Not reported"}.</p>
    {!supported && <p role="status">This hub has not reported adjustable BLE power. Update its firmware to enable power control. Advertising on/off is still available.</p>}
    {supported && steps !== 5 && <p role="status">Update this hub’s firmware to enable the additional Low and High settings.</p>}
    <p>The Home beacon advertises only while connected to primary Home Wi-Fi. Power is saved while advertising is off and applies when it resumes. Higher power may improve coverage; walls and antenna placement still matter.</p>
    {error && <p role="alert">{error}</p>}
    <div className="modal-actions"><button className="btn-primary" disabled={pending || offline}>Apply settings</button><button type="button" className="btn-secondary" onClick={onClose}>Close</button></div>
  </form></div>, document.body);
}
