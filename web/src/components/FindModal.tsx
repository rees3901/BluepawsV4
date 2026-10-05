"use client";
import { useState } from "react";
import { supportsLedFind, type LedFindAction } from "@/lib/ledFind";
export function FindModal({ device, sending, error, onClose, onSend }: {
  device: { id: number; name: string }; sending: boolean; error: string | null; onClose: () => void;
  onSend: (action: LedFindAction, minutes: number) => void;
}) {
  const [minutes, setMinutes] = useState(10);
  const supported = supportsLedFind(device.id);
  return <div className="modal" role="dialog" aria-modal="true" aria-labelledby="find-title">
    <div className="modal-content">
      <h2 id="find-title">Find Alert</h2><p>Device: <strong>{device.name}</strong></p>
      {!supported && <p role="status">LED Find requires compatible collar firmware. Currently enabled for the fitted personal batch (3001–3004).</p>}
      <p>Seven rapid LED flashes per cycle.</p>
      <button className="btn-primary" disabled={sending || !supported} onClick={() => onSend("flash", minutes)}>Flash now</button>
      <div className="form-group"><label htmlFor="find-duration">Repeat once every minute for</label>
        <select id="find-duration" value={minutes} disabled={sending || !supported} onChange={event => setMinutes(Number(event.target.value))}>
          {[1, 5, 10, 15, 30, 60].map(value => <option key={value} value={value}>{value} {value === 1 ? "minute" : "minutes"}</option>)}
        </select>
      </div>
      <p className="form-hint">Commands arrive on the next collar check-in and expire after ten minutes if unacknowledged. Compatible collar firmware is required. Repeat starts when received and stops automatically. Emergency Lost uses a minute LED cycle; Stop also silences that cycle until Lost mode is entered again.</p>
      {error && <p role="alert">{error}</p>}
      <div className="modal-actions">
        <button className="btn-primary" disabled={sending || !supported} onClick={() => onSend("repeat", minutes)}>{sending ? "Queueing…" : "Start repeating"}</button>
        <button className="btn-secondary" disabled={sending || !supported} onClick={() => onSend("stop", minutes)}>Stop LED cycle</button>
        <button className="btn-secondary" disabled={sending} onClick={onClose}>Close</button>
      </div>
    </div>
  </div>;
}
