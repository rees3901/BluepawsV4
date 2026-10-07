"use client";
import { useState } from "react";
import { LED_DURATIONS, ledIntervalsForProfile, ledTimeLabel, supportsLedFind, type LedFindAction } from "@/lib/ledFind";
export function FindModal({ device, sending, error, onClose, onSend }: {
  device: { id: number; name: string; profile?: string }; sending: boolean; error: string | null; onClose: () => void;
  onSend: (action: LedFindAction, seconds: number, interval: number) => void;
}) {
  const [seconds, setSeconds] = useState(600);
  const [interval, setInterval] = useState(60);
  const supported = supportsLedFind(device.id);
  const lost = device.profile === "Emergency Lost";
  const intervals = ledIntervalsForProfile(device.profile);
  // A live profile change must also invalidate a previously selected fast interval.
  const effectiveInterval = intervals.includes(interval as typeof intervals[number]) ? interval : 60;
  return <div className="modal" role="dialog" aria-modal="true" aria-labelledby="find-title">
    <div className="modal-content">
      <h2 id="find-title">Find Alert</h2><p>Device: <strong>{device.name}</strong></p>
      {!supported && <p role="status">LED Find requires compatible collar firmware. Currently enabled for the fitted personal batch (3001–3004).</p>}
      <p>Each cycle is seven rapid LED flashes in about one second.</p>
      {lost && <button className="btn-primary" disabled={sending || !supported} onClick={() => onSend("flash", seconds, effectiveInterval)}>Flash now</button>}
      <div className="form-group"><label htmlFor="find-interval">Flash one cycle every</label>
        <select id="find-interval" value={effectiveInterval} disabled={sending || !supported} onChange={event => setInterval(Number(event.target.value))}>
          {intervals.map(value => <option key={value} value={value}>{ledTimeLabel(value)}</option>)}
        </select>
      </div>
      {!lost && <p className="form-hint">Fast repeats and Flash now are available after the collar reports Emergency Lost mode. Other profiles sleep between cycles; repeat defaults to once a minute for ten minutes.</p>}
      <div className="form-group"><label htmlFor="find-duration">Keep flashing for</label>
        <select id="find-duration" value={seconds} disabled={sending || !supported} onChange={event => setSeconds(Number(event.target.value))}>
          {LED_DURATIONS.map(value => <option key={value} value={value}>{ledTimeLabel(value)}</option>)}
        </select>
      </div>
      <p className="form-hint">Commands arrive on the next collar check-in and expire after ten minutes if unacknowledged. Compatible collar firmware is required. Repeat starts when received and stops automatically. Emergency Lost uses a minute LED cycle; Stop also silences that cycle until Lost mode is entered again.</p>
      {error && <p role="alert">{error}</p>}
      <div className="modal-actions">
        <button className="btn-primary" disabled={sending || !supported} onClick={() => onSend("repeat", seconds, effectiveInterval)}>{sending ? "Queueing…" : "Start repeating"}</button>
        <button className="btn-secondary" disabled={sending || !supported} onClick={() => onSend("stop", seconds, interval)}>Stop LED cycle</button>
        <button className="btn-secondary" disabled={sending} onClick={onClose}>Close</button>
      </div>
    </div>
  </div>;
}
