"use client";

import dynamic from "next/dynamic";
import { useRouter } from "next/navigation";
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { DeviceCard, DownloadIcon } from "@/components/DeviceCard";
import { GuidedTour } from "@/components/GuidedTour";
import { createRealtimeTelemetrySource, loadDeviceTrail } from "@/lib/realtimeTelemetry";
import { createClient } from "@/lib/supabase/client";
import { getTutorialTelemetrySource } from "@/lib/telemetry";
import type { DeviceAction, DeviceAvatar, MapCommand, TelemetryConnectionStatus, TelemetryDevice, TrailPoint } from "@/types/telemetry";

const TrackingMap = dynamic(() => import("@/components/TrackingMap"), {
  ssr: false,
  loading: () => <div className="map-loading">Loading map…</div>,
});

const EMOJIS = ["🐱", "🐶", "🐰", "🐾", "🦊", "🐹", "🦉", "🐼"];
const COLORS = ["#1d9bf0", "#ff6b35", "#a855f7", "#22c55e", "#f97316", "#06b6d4", "#84cc16", "#ec4899"];
const HOME = { lat: 51.5055, lon: -0.09 };
const TUTORIAL_MODE_STORAGE_KEY = "bp_tutorial_mode";
const TUTORIAL_COMPLETE_STORAGE_KEY = "bp_tutorial_complete";
const TUTORIAL_PROMPT_STORAGE_KEY = "bp_tutorial_prompt_seen";

interface SelectedDevice {
  id: number;
  name: string;
}

interface DashboardProps {
  householdId: string | null;
  initialLiveDevices: TelemetryDevice[];
  liveTelemetryError: string | null;
  userEmail: string | null;
}

export function Dashboard({ householdId, initialLiveDevices, liveTelemetryError, userEmail }: DashboardProps) {
  const router = useRouter();
  const [devices, setDevices] = useState<TelemetryDevice[]>(initialLiveDevices);
  const [connected, setConnected] = useState(false);
  const [connectionStatus, setConnectionStatus] = useState<TelemetryConnectionStatus>("connecting");
  const [connectionDetail, setConnectionDetail] = useState<string | null>(null);
  const [preferencesReady, setPreferencesReady] = useState(false);
  const [tutorialMode, setTutorialMode] = useState(false);
  const [tutorialOpen, setTutorialOpen] = useState(false);
  const [tutorialPromptOpen, setTutorialPromptOpen] = useState(false);
  const [sidebarOpen, setSidebarOpen] = useState(true);
  const [darkMode, setDarkMode] = useState(true);
  const [expandedId, setExpandedId] = useState<number | null>(null);
  const [followedId, setFollowedId] = useState<number | null>(null);
  const [trailIds, setTrailIds] = useState<Set<number>>(() => new Set());
  const [trailHistory, setTrailHistory] = useState<Record<number, TrailPoint[]>>({});
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
    const restorePreferences = window.setTimeout(() => {
      try {
        setDarkMode(localStorage.getItem("bp_theme") !== "light");
        const tutorialEnabled = localStorage.getItem(TUTORIAL_MODE_STORAGE_KEY) === "true";
        const tutorialComplete = localStorage.getItem(TUTORIAL_COMPLETE_STORAGE_KEY) === "true";
        setTutorialMode(tutorialEnabled);
        setTutorialOpen(tutorialEnabled && !tutorialComplete);
        setTutorialPromptOpen(!tutorialEnabled && !tutorialComplete && localStorage.getItem(TUTORIAL_PROMPT_STORAGE_KEY) !== "true");
      } catch { /* localStorage can be unavailable in privacy modes */ }
      setPreferencesReady(true);
    }, 0);
    const clock = window.setInterval(() => setNow(Date.now()), 1000);
    return () => {
      window.clearTimeout(restorePreferences);
      window.clearInterval(clock);
    };
  }, []);

  useEffect(() => {
    if (!preferencesReady) return;

    if (!tutorialMode && !householdId) {
      return;
    }

    let fittedInitialPayload = false;
    const source = tutorialMode
      ? getTutorialTelemetrySource()
      : createRealtimeTelemetrySource(householdId as string, initialLiveDevices);
    const unsubscribe = source.subscribe((incoming) => {
      setDevices(incoming);
      if (!fittedInitialPayload && incoming.length > 0) {
        fittedInitialPayload = true;
        setMapCommand({ type: "fit", nonce: Date.now() });
      }
      const newLines: string[] = [];
      incoming.forEach((device) => {
        if (sequences.current.get(device.id) !== device.seq) {
          sequences.current.set(device.id, device.seq);
          const signal = device.rssi === null ? "not-reported" : `${device.rssi}dBm`;
          const battery = device.batteryPercent === undefined || device.batteryPercent === null ? `${device.batt}mV` : `${device.batteryPercent}%`;
          newLines.push(`[${new Date().toLocaleTimeString()}] RX ${device.name} id=${device.id} lat=${device.lat.toFixed(5)} lon=${device.lon.toFixed(5)} rssi=${signal} batt=${battery}`);
        }
      });
      if (newLines.length) setLogs((current) => [...newLines, ...current].slice(0, 200));
    }, (status, detail) => {
      setConnectionStatus(status);
      setConnected(status === "connected");
      setConnectionDetail(detail ?? null);
    });
    return unsubscribe;
  }, [householdId, initialLiveDevices, preferencesReady, tutorialMode]);

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
      if (next.has(device.id)) {
        next.delete(device.id);
      } else {
        next.add(device.id);
        if (!tutorialMode && !trailHistory[device.id]) {
          void loadDeviceTrail(device.id)
            .then((points) => setTrailHistory((history) => ({ ...history, [device.id]: points })))
            .catch(() => setToast("Unable to load this device's seven-day trail"));
        }
      }
      return next;
    });
    if (action === "find") setFindDevice({ id: device.id, name: device.name });
    if (action === "command") setCommandDevice({ id: device.id, name: device.name });
  }, [trailHistory, tutorialMode]);

  const handleTutorialModeChange = useCallback((enabled: boolean) => {
    try {
      localStorage.setItem(TUTORIAL_MODE_STORAGE_KEY, String(enabled));
      localStorage.removeItem("bp_demo_mode");
      if (enabled) {
        localStorage.removeItem(TUTORIAL_COMPLETE_STORAGE_KEY);
        localStorage.setItem(TUTORIAL_PROMPT_STORAGE_KEY, "true");
      }
    } catch { /* non-critical preference */ }
    setConnected(false);
    setDevices([]);
    setLogs([]);
    sequences.current.clear();
    setExpandedId(null);
    setFollowedId(null);
    setTrailIds(new Set());
    setTrailHistory({});
    setSettingsOpen(false);
    setTutorialPromptOpen(false);
    setSidebarOpen(true);
    setTutorialMode(enabled);
    setTutorialOpen(enabled);
  }, []);

  const dismissTutorialPrompt = useCallback(() => {
    try { localStorage.setItem(TUTORIAL_PROMPT_STORAGE_KEY, "true"); } catch { /* non-critical preference */ }
    setTutorialPromptOpen(false);
  }, []);

  const startTutorialFromPrompt = useCallback(() => {
    handleTutorialModeChange(true);
  }, [handleTutorialModeChange]);

  const finishTutorial = useCallback((completed: boolean) => {
    try { localStorage.setItem(TUTORIAL_COMPLETE_STORAGE_KEY, "true"); } catch { /* non-critical preference */ }
    setTutorialOpen(false);
    setToast(completed ? "Tutorial complete" : "Tutorial skipped — replay it from Settings anytime");
  }, []);

  const replayTutorial = useCallback(() => {
    setSettingsOpen(false);
    setSidebarOpen(true);
    setExpandedId(null);
    setTutorialOpen(true);
  }, []);

  const firstTutorialDeviceId = devices[0]?.id;
  const handleTutorialStepChange = useCallback((step: number) => {
    if (step >= 1 && step <= 3) setSidebarOpen(true);
    if (step === 3 && firstTutorialDeviceId !== undefined) setExpandedId(firstTutorialDeviceId);
  }, [firstTutorialDeviceId]);
  const completeTutorial = useCallback(() => finishTutorial(true), [finishTutorial]);
  const skipTutorial = useCallback(() => finishTutorial(false), [finishTutorial]);

  const effectiveError = connectionDetail ?? liveTelemetryError;
  const liveUnavailable = !tutorialMode && effectiveError !== null && devices.length === 0;
  const statusClass = connected ? (tutorialMode ? "tutorial" : "connected") : "waiting";
  const statusText = liveUnavailable
    ? "Unavailable"
    : connectionStatus === "degraded"
      ? "Reconnecting"
      : connected
        ? (tutorialMode ? "Tutorial" : "Live")
        : "Connecting";
  const emptyTitle = !householdId && !tutorialMode
    ? "Account setup incomplete"
    : liveUnavailable
      ? "Live telemetry unavailable"
      : connected
        ? "Connected to Supabase"
        : "Waiting for live telemetry";
  const emptyMessage = !householdId && !tutorialMode
    ? "No household is assigned to this account yet."
    : liveUnavailable
      ? effectiveError
      : "No devices have reported yet.";

  const handleSignOut = useCallback(async () => {
    const supabase = createClient();
    await supabase.auth.signOut();
    router.replace("/login");
    router.refresh();
  }, [router]);

  return (
    <>
      <button className="hamburger-btn" title="Toggle sidebar" aria-label="Toggle sidebar" onClick={() => setSidebarOpen((open) => !open)}>
        <svg width="20" height="20" viewBox="0 0 20 20" fill="currentColor"><rect x="2" y="4" width="16" height="2" rx="1" /><rect x="2" y="9" width="16" height="2" rx="1" /><rect x="2" y="14" width="16" height="2" rx="1" /></svg>
      </button>

      <aside id="panel" className={sidebarOpen ? "open" : ""}>
        <div id="panelHeader">
          <span className="panel-title">Bluepaws V4</span>
          <div className="panel-header-btns">
            <span id="statusBanner" className={statusClass}>
              <span id="statusIcon">●</span><span id="statusText">{statusText}</span>
            </span>
            <button className="ctrl-btn" title="Toggle dark/light theme" aria-label="Toggle theme" onClick={() => setDarkMode((dark) => !dark)}>
              {darkMode ? <MoonIcon /> : <SunIcon />}
            </button>
            <button className="ctrl-btn" data-tour="settings" title="Settings" aria-label="Settings" onClick={() => setSettingsOpen(true)}><SettingsIcon /></button>
          </div>
        </div>
        {tutorialMode && <div className="tutorial-mode-banner">TUTORIAL MODE — SIMULATED DATA</div>}
        {portableMode && <div className="portable-banner">PORTABLE MODE</div>}
        <div id="deviceCards">
          {devices.length === 0 && (
            <div className="telemetry-empty-state" role="status">
              <span className="telemetry-empty-icon" aria-hidden="true">⌁</span>
              <strong>{emptyTitle}</strong>
              <span>{emptyMessage}</span>
              <button type="button" onClick={() => setSettingsOpen(true)}>Open Settings</button>
            </div>
          )}
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

      <TrackingMap devices={devices} avatars={avatars} sidebarOpen={sidebarOpen} followedId={followedId} trailIds={trailIds} trailHistory={trailHistory} command={mapCommand} onAction={handleAction} />

      {settingsOpen && (
        <SettingsModal
          logs={logs}
          portableMode={portableMode}
          devices={devices.length}
          tutorialMode={tutorialMode}
          connected={connected}
          liveTelemetryError={liveTelemetryError}
          connectionDetail={connectionDetail}
          userEmail={userEmail}
          onTutorialModeChange={handleTutorialModeChange}
          onReplayTutorial={replayTutorial}
          onModeChange={setPortableMode}
          onSignOut={handleSignOut}
          onClose={() => setSettingsOpen(false)}
        />
      )}
      {tutorialPromptOpen && (
        <TutorialWelcomeCard onStart={startTutorialFromPrompt} onDismiss={dismissTutorialPrompt} />
      )}
      {commandDevice && <CommandModal device={commandDevice} onClose={() => setCommandDevice(null)} onSend={(mode) => { setCommandDevice(null); setToast(tutorialMode ? `Tutorial command preview: ${mode}` : "Command sending is not connected yet"); }} />}
      {findDevice && <FindModal device={findDevice} onClose={() => setFindDevice(null)} onSend={() => { setFindDevice(null); setToast(tutorialMode ? "Tutorial Find Alert previewed" : "Find Alert sending is not connected yet"); }} />}
      {tutorialOpen && tutorialMode && (
        <GuidedTour
          onFinish={completeTutorial}
          onSkip={skipTutorial}
          onStepChange={handleTutorialStepChange}
        />
      )}
      {toast && <div className="tutorial-toast" role="status">{toast}</div>}
    </>
  );
}

interface SettingsModalProps {
  logs: string[];
  portableMode: boolean;
  devices: number;
  tutorialMode: boolean;
  connected: boolean;
  liveTelemetryError: string | null;
  connectionDetail: string | null;
  userEmail: string | null;
  onTutorialModeChange: (enabled: boolean) => void;
  onReplayTutorial: () => void;
  onModeChange: (portable: boolean) => void;
  onSignOut: () => void;
  onClose: () => void;
}

function SettingsModal({ logs, portableMode, devices, tutorialMode, connected, liveTelemetryError, connectionDetail, userEmail, onTutorialModeChange, onReplayTutorial, onModeChange, onSignOut, onClose }: SettingsModalProps) {
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
        <div className="form-group"><label>Hub Status</label><div className="status-info">{tutorialMode ? "Tutorial mode" : "Live mode"}<br />Devices: {devices}<br />Data source: {tutorialMode ? "Simulated tutorial telemetry" : "Private Supabase household channel"}<br />Cloud: {tutorialMode ? "Disabled in Tutorial Mode" : connected ? "Realtime connected" : connectionDetail ?? liveTelemetryError ?? "Connecting to Supabase"}</div></div>
        <div className="form-group"><label>Account</label><div className="status-info">{userEmail ?? "Signed-in Bluepaws user"}</div><button className="btn-secondary account-signout" type="button" onClick={onSignOut}>Sign out</button></div>
        <div className="form-group">
          <div className="log-btn-row">
            <button className="btn-secondary" style={{ flex: 1 }} onClick={() => setConsoleOpen((open) => !open)}>Console Log</button>
            <button className="btn-log-export" title="Export log as CSV" aria-label="Export console log" onClick={() => downloadLog(logs)}><DownloadIcon /></button>
          </div>
          {consoleOpen && <div><pre className="console-log">{logs.join("\n") || "No messages yet."}</pre></div>}
        </div>
        <p className="tutorial-note">These hub-specific fields are preserved for interface parity. Live mode reads the latest device reports from Supabase; Tutorial Mode remains isolated from customer data.</p>
        <div className="tutorial-mode-setting">
          <Toggle label="Tutorial Mode" checked={tutorialMode} onChange={onTutorialModeChange} />
          <p className="form-hint">Off by default. Loads simulated animals and a guided tour without mixing in live data.</p>
          {tutorialMode && <button className="tutorial-replay-btn" type="button" onClick={onReplayTutorial}>Replay tutorial</button>}
        </div>
        <div className="modal-actions"><button className="btn-primary" disabled>Save &amp; Restart</button><button className="btn-secondary" onClick={onClose}>Close</button></div>
      </div>
    </div>
  );
}

function TutorialWelcomeCard({ onStart, onDismiss }: { onStart: () => void; onDismiss: () => void }) {
  return (
    <aside className="tutorial-welcome-card" role="dialog" aria-labelledby="tutorial-welcome-title" aria-describedby="tutorial-welcome-description" onKeyDown={(event) => { if (event.key === "Escape") onDismiss(); }}>
      <button className="tutorial-welcome-close" type="button" aria-label="Dismiss tutorial invitation" title="Dismiss" onClick={onDismiss}>×</button>
      <span className="tutorial-welcome-eyebrow">New to Bluepaws?</span>
      <h2 id="tutorial-welcome-title">Would you like a quick tour?</h2>
      <p id="tutorial-welcome-description">Explore the dashboard with five simulated pets and learn the essential controls in about a minute.</p>
      <p className="tutorial-welcome-hint">You can start it later from Settings.</p>
      <div className="modal-actions">
        <button className="btn-primary" type="button" onClick={onStart}>Start tutorial</button>
        <button className="btn-secondary" type="button" onClick={onDismiss}>Not now</button>
      </div>
    </aside>
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
  return <div className="form-group"><div className="toggle-row"><label>{label}</label><label className="toggle-switch"><input type="checkbox" aria-label={label} checked={checked} onChange={(event) => onChange(event.target.checked)} /><span className="toggle-slider" /></label></div></div>;
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
