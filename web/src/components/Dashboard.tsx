"use client";

import dynamic from "next/dynamic";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { DeviceCard, DownloadIcon } from "@/components/DeviceCard";
import { telemetrySource } from "@/lib/telemetry";
import type { DeviceAction, DeviceAvatar, MapCommand, TelemetryDevice } from "@/types/telemetry";

const TrackingMap = dynamic(() => import("@/components/TrackingMap"), {
  ssr: false,
  loading: () => <div className="map-loading">Loading map…</div>,
});

const EMOJIS = ["🐱", "🐶", "🐰", "🐾", "🦊", "🐹", "🦉", "🐼"];
const COLORS = ["#1d9bf0", "#ff6b35", "#a855f7", "#22c55e", "#f97316", "#06b6d4", "#84cc16", "#ec4899"];
const HOME = { lat: 51.5055, lon: -0.09 };

interface SelectedDevice {
  id: number;
  name: string;
}

export function Dashboard() {
  const [devices, setDevices] = useState<TelemetryDevice[]>([]);
  const [connected, setConnected] = useState(false);
  const [sidebarOpen, setSidebarOpen] = useState(true);
  const [darkMode, setDarkMode] = useState(true);
  const [expandedId, setExpandedId] = useState<number | null>(null);
  const [followedId, setFollowedId] = useState<number | null>(null);
  const [trailIds, setTrailIds] = useState<Set<number>>(() => new Set());
  const [portableMode, setPortableMode] = useState(false);
  const [mapCommand, setMapCommand] = useState<MapCommand | null>(null);
  const [settingsOpen, setSettingsOpen] = useState(false);
  const [commandDevice, setCommandDevice] = useState<SelectedDevice | null>(null);
  const [findDevice, setFindDevice] = useState<SelectedDevice | null>(null);
  const [toast, setToast] = useState<string | null>(null);
  const [now, setNow] = useState(0);
  const [logs, setLogs] = useState<string[]>([]);
  const sequences = useRef(new Map<number, number>());

  const avatars = useMemo<Record<number, DeviceAvatar>>(() => Object.fromEntries(devices.map((device, index) => [device.id, { emoji: EMOJIS[index % EMOJIS.length], color: COLORS[index % COLORS.length] }])), [devices]);

  useEffect(() => {
    const restoreTheme = window.setTimeout(() => {
      try { setDarkMode(localStorage.getItem("bp_theme") !== "light"); } catch { /* localStorage can be unavailable in privacy modes */ }
    }, 0);
    const unsubscribe = telemetrySource.subscribe((incoming) => {
      setConnected(true);
      setDevices(incoming);
      const newLines: string[] = [];
      incoming.forEach((device) => {
        if (sequences.current.get(device.id) !== device.seq) {
          sequences.current.set(device.id, device.seq);
          newLines.push(`[${new Date().toLocaleTimeString()}] RX ${device.name} id=${device.id} lat=${device.lat.toFixed(5)} lon=${device.lon.toFixed(5)} rssi=${device.rssi} batt=${device.batt}mV`);
        }
      });
      if (newLines.length) setLogs((current) => [...newLines, ...current].slice(0, 200));
    });
    const clock = window.setInterval(() => setNow(Date.now()), 1000);
    return () => {
      unsubscribe();
      window.clearTimeout(restoreTheme);
      window.clearInterval(clock);
    };
  }, []);

  useEffect(() => {
    document.body.classList.toggle("panel-open", sidebarOpen);
    document.body.classList.toggle("light", !darkMode);
    try { localStorage.setItem("bp_theme", darkMode ? "dark" : "light"); } catch { /* non-critical preference */ }
  }, [darkMode, sidebarOpen]);

  useEffect(() => {
    if (!toast) return;
    const timer = window.setTimeout(() => setToast(null), 2600);
    return () => window.clearTimeout(timer);
  }, [toast]);

  const handleAction = useCallback((device: TelemetryDevice, action: DeviceAction) => {
    if (action === "jump") setMapCommand({ type: "jump", deviceId: device.id, nonce: Date.now() });
    if (action === "follow") setFollowedId((current) => current === device.id ? null : device.id);
    if (action === "trail") setTrailIds((current) => {
      const next = new Set(current);
      if (next.has(device.id)) next.delete(device.id); else next.add(device.id);
      return next;
    });
    if (action === "find") setFindDevice({ id: device.id, name: device.name });
    if (action === "command") setCommandDevice({ id: device.id, name: device.name });
  }, []);

  return (
    <>
      <button className="hamburger-btn" title="Toggle sidebar" aria-label="Toggle sidebar" onClick={() => setSidebarOpen((open) => !open)}>
        <svg width="20" height="20" viewBox="0 0 20 20" fill="currentColor"><rect x="2" y="4" width="16" height="2" rx="1" /><rect x="2" y="9" width="16" height="2" rx="1" /><rect x="2" y="14" width="16" height="2" rx="1" /></svg>
      </button>

      <aside id="panel" className={sidebarOpen ? "open" : ""}>
        <div id="panelHeader">
          <span className="panel-title">Bluepaws V4</span>
          <div className="panel-header-btns">
            <span id="statusBanner" className={connected ? "connected" : "disconnected"}>
              <span id="statusIcon">●</span><span id="statusText">{connected ? "Connected" : "Connecting…"}</span>
            </span>
            <button className="ctrl-btn" title="Toggle dark/light theme" aria-label="Toggle theme" onClick={() => setDarkMode((dark) => !dark)}>
              {darkMode ? <MoonIcon /> : <SunIcon />}
            </button>
            <button className="ctrl-btn" title="Settings" aria-label="Settings" onClick={() => setSettingsOpen(true)}><SettingsIcon /></button>
          </div>
        </div>
        {portableMode && <div className="portable-banner">PORTABLE MODE</div>}
        <div id="deviceCards">
          {devices.map((device) => (
            <DeviceCard
              key={device.id}
              device={device}
              avatar={avatars[device.id]}
              expanded={expandedId === device.id}
              followed={followedId === device.id}
              trailVisible={trailIds.has(device.id)}
              portableMode={portableMode}
              distance={formatDistance(haversine(HOME.lat, HOME.lon, device.lat, device.lon))}
              ageSeconds={Math.max(0, Math.floor((now - device.lastUpdate) / 1000))}
              onExpand={() => setExpandedId((current) => current === device.id ? null : device.id)}
              onAction={(action) => handleAction(device, action)}
            />
          ))}
        </div>
      </aside>

      <TrackingMap devices={devices} avatars={avatars} sidebarOpen={sidebarOpen} followedId={followedId} trailIds={trailIds} command={mapCommand} onAction={handleAction} />

      {settingsOpen && (
        <SettingsModal
          logs={logs}
          portableMode={portableMode}
          devices={devices.length}
          onModeChange={setPortableMode}
          onClose={() => setSettingsOpen(false)}
        />
      )}
      {commandDevice && <CommandModal device={commandDevice} onClose={() => setCommandDevice(null)} onSend={(mode) => { setCommandDevice(null); setToast(`Demo command queued: ${mode}`); }} />}
      {findDevice && <FindModal device={findDevice} onClose={() => setFindDevice(null)} onSend={() => { setFindDevice(null); setToast("Demo Find Alert queued"); }} />}
      {toast && <div className="demo-toast" role="status">{toast}</div>}
    </>
  );
}

function SettingsModal({ logs, portableMode, devices, onModeChange, onClose }: { logs: string[]; portableMode: boolean; devices: number; onModeChange: (portable: boolean) => void; onClose: () => void }) {
  const [consoleOpen, setConsoleOpen] = useState(false);
  return (
    <div className="modal" role="dialog" aria-modal="true" aria-labelledby="settings-title">
      <div className="modal-content">
        <h2 id="settings-title">Hub Settings</h2>
        <div className="form-group"><label htmlFor="cfgSSID">WiFi SSID</label><input id="cfgSSID" type="text" placeholder="Home network name" /></div>
        <div className="form-group"><label htmlFor="cfgPass">WiFi Password</label><input id="cfgPass" type="password" placeholder="Password" /></div>
        <div className="form-group"><label htmlFor="cfgCloud">Cloud Endpoint</label><input id="cfgCloud" type="url" placeholder="https://..." /></div>
        <div className="form-group">
          <label>Hub Mode</label>
          <div className="toggle-row">
            <button className={`mode-btn${portableMode ? "" : " active"}`} onClick={() => onModeChange(false)}>Home</button>
            <button className={`mode-btn${portableMode ? " active" : ""}`} onClick={() => onModeChange(true)}>Portable Locator</button>
          </div>
          <small className="form-hint">Portable mode stops the home beacon and scans for collar BLE signals.</small>
        </div>
        <div className="form-group"><label>Hub Status</label><div className="status-info">Live demo<br />Devices: {devices}<br />Data source: Mock telemetry<br />Cloud: Not connected</div></div>
        <div className="form-group">
          <div className="log-btn-row">
            <button className="btn-secondary" style={{ flex: 1 }} onClick={() => setConsoleOpen((open) => !open)}>Console Log</button>
            <button className="btn-log-export" title="Export log as CSV" aria-label="Export console log" onClick={() => downloadLog(logs)}><DownloadIcon /></button>
          </div>
          {consoleOpen && <div><pre className="console-log">{logs.join("\n") || "No messages yet."}</pre></div>}
        </div>
        <p className="demo-note">These hub-specific fields are preserved for interface parity. Customer configuration and Supabase connectivity will be introduced in a later integration phase.</p>
        <div className="modal-actions"><button className="btn-primary" disabled>Save &amp; Restart</button><button className="btn-secondary" onClick={onClose}>Close</button></div>
      </div>
    </div>
  );
}

function CommandModal({ device, onClose, onSend }: { device: SelectedDevice; onClose: () => void; onSend: (mode: string) => void }) {
  const [mode, setMode] = useState("normal");
  return (
    <div className="modal" role="dialog" aria-modal="true" aria-labelledby="command-title">
      <div className="modal-content">
        <h2 id="command-title">Send Command</h2><p>Device: <strong>{device.name}</strong></p>
        <div className="form-group"><label htmlFor="cmdMode">Change Mode</label><select id="cmdMode" value={mode} onChange={(event) => setMode(event.target.value)}><option value="normal">Normal</option><option value="powersave">PowerSave</option><option value="active_find">Active Find</option><option value="emergency_lost">Emergency Lost</option></select></div>
        <div className="modal-actions"><button className="btn-primary" onClick={() => onSend(mode)}>Send</button><button className="btn-secondary" onClick={onClose}>Cancel</button></div>
      </div>
    </div>
  );
}

function FindModal({ device, onClose, onSend }: { device: SelectedDevice; onClose: () => void; onSend: () => void }) {
  const [buzzer, setBuzzer] = useState(true);
  const [led, setLed] = useState(true);
  const [duration, setDuration] = useState(5);
  return (
    <div className="modal" role="dialog" aria-modal="true" aria-labelledby="find-title">
      <div className="modal-content">
        <h2 id="find-title">Find Alert</h2><p>Device: <strong>{device.name}</strong></p>
        <Toggle label="Buzzer" checked={buzzer} onChange={setBuzzer} />
        <div className="form-group"><label htmlFor="findPattern">Buzzer Pattern</label><select id="findPattern" disabled={!buzzer}><option>Chirp (3 short beeps)</option><option>Trill (rising tone)</option><option>Siren (two-tone alternating)</option><option>Melody A</option><option>Melody B</option></select></div>
        <Toggle label="LED Flash" checked={led} onChange={setLed} />
        <div className="form-group"><label htmlFor="findFlash">LED Pattern</label><select id="findFlash" disabled={!led} defaultValue="5"><option value="3">3 flashes per cycle</option><option value="5">5 flashes per cycle</option><option value="10">10 flashes per cycle (rapid)</option></select></div>
        <div className="form-group"><label>Alert Duration</label><div className="duration-selector"><button className="btn-inc" title="Decrease" onClick={() => setDuration((value) => Math.max(1, value - 1))}>▼</button><span className="dur-value">{duration}</span><span className="dur-unit">min</span><button className="btn-inc" title="Increase" onClick={() => setDuration((value) => Math.min(60, value + 1))}>▲</button></div></div>
        <div className="modal-actions"><button className="btn-primary btn-find-send" disabled={!buzzer && !led} onClick={onSend}>Send Alert</button><button className="btn-secondary" onClick={onClose}>Cancel</button></div>
      </div>
    </div>
  );
}

function Toggle({ label, checked, onChange }: { label: string; checked: boolean; onChange: (checked: boolean) => void }) {
  return <div className="form-group"><div className="toggle-row"><label>{label}</label><label className="toggle-switch"><input type="checkbox" checked={checked} onChange={(event) => onChange(event.target.checked)} /><span className="toggle-slider" /></label></div></div>;
}

function MoonIcon() { return <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor"><path d="M12 3a9 9 0 1 0 9 9c0-.46-.04-.92-.1-1.36a5.389 5.389 0 0 1-4.4 2.26 5.403 5.403 0 0 1-3.14-9.8c-.44-.06-.9-.1-1.36-.1z" /></svg>; }
function SunIcon() { return <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="12" r="4" /><path d="M12 1v3m0 16v3M1 12h3m16 0h3M4.2 4.2l2.1 2.1m11.4 11.4l2.1 2.1m0-15.6l-2.1 2.1M6.3 17.7l-2.1 2.1" stroke="currentColor" strokeWidth="2" /></svg>; }
function SettingsIcon() { return <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor"><path d="M12 15.5A3.5 3.5 0 1 1 12 8.5a3.5 3.5 0 0 1 0 7m7.43-2.53c.04-.32.07-.64.07-.97s-.03-.66-.07-1l2.11-1.63c.19-.15.24-.42.12-.64l-2-3.46a.5.5 0 0 0-.61-.22l-2.49 1a8 8 0 0 0-1.69-.98l-.38-2.65A.5.5 0 0 0 14 2h-4a.5.5 0 0 0-.49.42l-.38 2.65c-.61.25-1.17.59-1.69.98l-2.49-1a.5.5 0 0 0-.61.22l-2 3.46a.5.5 0 0 0 .12.64L4.57 11c-.04.34-.07.67-.07 1s.03.65.07.97l-2.11 1.66a.5.5 0 0 0-.12.64l2 3.46c.12.22.39.3.61.22l2.49-1.01c.52.4 1.08.73 1.69.98l.38 2.65c.03.24.24.42.49.42h4c.25 0 .46-.18.49-.42l.38-2.65c.61-.25 1.17-.58 1.69-.98l2.49 1.01c.22.08.49 0 .61-.22l2-3.46a.5.5 0 0 0-.12-.64z" /></svg>; }

function haversine(lat1: number, lon1: number, lat2: number, lon2: number) {
  const radius = 6371000;
  const dLat = (lat2 - lat1) * Math.PI / 180;
  const dLon = (lon2 - lon1) * Math.PI / 180;
  const value = Math.sin(dLat / 2) ** 2 + Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) * Math.sin(dLon / 2) ** 2;
  return radius * 2 * Math.atan2(Math.sqrt(value), Math.sqrt(1 - value));
}

function formatDistance(metres: number) { return metres >= 2000 ? `${(metres / 1000).toFixed(1)} km` : `${Math.round(metres)} m`; }

function downloadLog(logs: string[]) {
  const blob = new Blob([["timestamp,message", ...logs.map((line) => `\"${line.replaceAll('"', '""')}\"`)].join("\n")], { type: "text/csv;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const anchor = document.createElement("a");
  anchor.href = url;
  anchor.download = `bluepaws_console_${new Date().toISOString().slice(0, 10)}.csv`;
  anchor.click();
  URL.revokeObjectURL(url);
}
