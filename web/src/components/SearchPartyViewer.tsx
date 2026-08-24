"use client";

/* eslint-disable @next/next/no-img-element -- Tiny pre-sized emoji artwork is intentionally served directly from the picker CDN. */
import dynamic from "next/dynamic";
import { useCallback, useEffect, useMemo, useState } from "react";
import { BatteryIndicator, HomeDistance, LastSeen, SignalIndicator } from "@/components/Indicators";
import { defaultDeviceAvatar } from "@/lib/defaultDeviceAvatar";
import { emojiImageUrl } from "@/lib/emoji";
import { formatMapCoordinates, googleMapsUrl } from "@/lib/mapLocation";
import type { SearchPartySnapshot } from "@/lib/searchParty";
import type { DeviceAction, DeviceAvatar, MapCommand, TelemetryDevice, TrailPoint } from "@/types/telemetry";

const TrackingMap = dynamic(() => import("@/components/TrackingMap"), {
  ssr: false,
  loading: () => <div className="map-loading">Loading search map…</div>,
});

const REFRESH_INTERVAL_MS = 10_000;
const HOME = { lat: 51.5055, lon: -0.09 };

const STATUS = {
  home: { emoji: "🏠", label: "Home", css: "status-home" },
  out: { emoji: "🐾", label: "Out", css: "status-out" },
  lost: { emoji: "‼", label: "Lost", css: "status-lost" },
  error: { emoji: "❓", label: "Error", css: "status-error" },
};

interface SearchPartyViewerProps {
  token: string;
  initialSnapshot: SearchPartySnapshot;
}

export function SearchPartyViewer({ token, initialSnapshot }: SearchPartyViewerProps) {
  const [snapshot, setSnapshot] = useState(initialSnapshot);
  const [lastRefresh, setLastRefresh] = useState<Date | null>(() => new Date());
  const [now, setNow] = useState(() => Date.now());
  const [refreshError, setRefreshError] = useState<string | null>(null);
  const [mapCommand, setMapCommand] = useState<MapCommand | null>(null);
  const [panelOpen, setPanelOpen] = useState(true);

  const refresh = useCallback(async () => {
    try {
      const response = await fetch(`/api/search-party/${encodeURIComponent(token)}`, {
        cache: "no-store",
        headers: { accept: "application/json" },
      });
      const next = await response.json() as SearchPartySnapshot;
      setSnapshot(next);
      setNow(Date.now());
      setLastRefresh(new Date());
      setRefreshError(response.ok ? null : next.error ?? "The search-party link is no longer available.");
    } catch {
      setRefreshError("Unable to refresh the search-party map. Check your connection.");
    }
  }, [token]);

  useEffect(() => {
    const timer = window.setInterval(() => { void refresh(); }, REFRESH_INTERVAL_MS);
    return () => window.clearInterval(timer);
  }, [refresh]);

  useEffect(() => {
    if (snapshot.devices.length === 0) return;
    const timer = window.setTimeout(() => setMapCommand({ type: "fit", nonce: Date.now() }), 250);
    return () => window.clearTimeout(timer);
  }, [snapshot.devices.length]);

  useEffect(() => {
    document.body.classList.toggle("panel-open", panelOpen && snapshot.valid);
    return () => document.body.classList.remove("panel-open");
  }, [panelOpen, snapshot.valid]);

  const devices = useMemo(() => snapshot.valid ? snapshot.devices : [], [snapshot.devices, snapshot.valid]);
  const avatars = useMemo<Record<number, DeviceAvatar>>(() => Object.fromEntries(devices.map((device) => [
    device.id,
    snapshot.avatars[device.id] ?? defaultDeviceAvatar(device.id),
  ])), [devices, snapshot.avatars]);

  const handleAction = useCallback((device: TelemetryDevice, action: DeviceAction) => {
    if (action === "jump") setMapCommand({ type: "jump", deviceId: device.id, nonce: Date.now() });
  }, []);

  const expiresText = snapshot.expiresAt ? new Date(snapshot.expiresAt).toLocaleString() : "soon";
  const refreshedText = lastRefresh ? lastRefresh.toLocaleTimeString() : "loading";

  if (!snapshot.valid) {
    return (
      <main className="search-party-shell invalid">
        <section className="search-party-invalid-card">
          <span className="login-brand">Bluepaws V4</span>
          <h1>Search link unavailable</h1>
          <p>This search-party link is invalid, expired or has been revoked by the Family Owner.</p>
          <p>Ask the Owner to create a fresh link if the search is still active.</p>
        </section>
      </main>
    );
  }

  return (
    <main className="search-party-shell">
      <button className="hamburger-btn" title="Toggle search-party panel" aria-label="Toggle search-party panel" onClick={() => setPanelOpen((open) => !open)}>
        <svg width="20" height="20" viewBox="0 0 20 20" fill="currentColor"><rect x="2" y="4" width="16" height="2" rx="1" /><rect x="2" y="9" width="16" height="2" rx="1" /><rect x="2" y="14" width="16" height="2" rx="1" /></svg>
      </button>
      <TrackingMap
        devices={devices}
        avatars={avatars}
        sidebarOpen={panelOpen}
        followedId={null}
        trailIds={new Set<number>()}
        trailHistory={{} as Record<number, TrailPoint[]>}
        command={mapCommand}
        onAction={handleAction}
        readOnly
      />
      <aside id="panel" className={`search-party-panel${panelOpen ? " open" : ""}`} aria-label="Search-party map details">
        <div id="panelHeader">
          <span className="panel-title">Bluepaws V4</span>
          <div className="panel-header-btns">
            <span id="statusBanner" className="connected">
              <span id="statusIcon">●</span><span id="statusText">Read-only</span>
            </span>
          </div>
        </div>
        <div className="search-party-panel-body">
          <section className="search-party-summary-card">
            <span className="settings-eyebrow">Search party map</span>
            <h1>{snapshot.familyName}</h1>
            <p>Read-only helper view. Positions refresh every 10 seconds; collar commands and account settings are unavailable.</p>
            <dl className="search-party-meta">
              <div><dt>Expires</dt><dd>{expiresText}</dd></div>
              <div><dt>Last refresh</dt><dd>{refreshedText}</dd></div>
            </dl>
            {refreshError && <p className="settings-message error" role="alert">{refreshError}</p>}
          </section>
          <div id="deviceCards" className="search-party-device-list">
            {devices.length === 0 ? (
              <p className="settings-copy">No pets have reported yet.</p>
            ) : devices.map((device) => (
              <SearchPartyDeviceRow
                key={device.id}
                device={device}
                avatar={avatars[device.id] ?? defaultDeviceAvatar(device.id)}
                now={now}
                onCentre={() => setMapCommand({ type: "jump", deviceId: device.id, nonce: Date.now() })}
              />
            ))}
          </div>
        </div>
      </aside>
    </main>
  );
}

function SearchPartyDeviceRow({ device, avatar, now, onCentre }: { device: TelemetryDevice; avatar: DeviceAvatar; now: number; onCentre: () => void }) {
  const mapsUrl = googleMapsUrl(device.lat, device.lon);
  const ageSeconds = Math.max(0, Math.floor((now - device.lastUpdate) / 1000));
  const status = STATUS[device.status.toLowerCase() as keyof typeof STATUS] ?? STATUS.error;
  const profileLower = device.profile.toLowerCase();
  const profileClass = `profile-${profileLower.replace("save", "").replaceAll(" ", "-")}`;
  const profileLabel = profileLower === "powersave" ? "💤 PowerSave" : profileLower === "debug" ? "🧪 Debug" : device.profile;
  const distance = formatDistance(haversine(HOME.lat, HOME.lon, device.lat, device.lon));

  return (
    <article className={`device-card search-party-device-card${ageSeconds > 600 ? " stale" : ""}`}>
      <div className="card-summary">
        <div className="card-avatar-wrap">
          <div
            className={`card-avatar${avatar.kind === "photo" ? " has-photo" : ""}`}
            style={{ borderColor: avatar.color, backgroundImage: avatar.photoUrl ? `url(${JSON.stringify(avatar.photoUrl)})` : undefined }}
            aria-hidden="true"
          >
            {avatar.kind === "photo" && avatar.photoUrl ? null : (
              <img className="avatar-emoji-image" src={emojiImageUrl(avatar.emoji)} alt={avatar.emoji} draggable={false} />
            )}
          </div>
        </div>
        <div className="card-identity">
          <div className="card-name-row">
            <span className="card-name">{device.name}</span>
            <span className={`card-status ${status.css}`}>{status.emoji} {status.label}</span>
            <span className={`card-profile ${profileClass}`}>{profileLabel}</span>
          </div>
          <div className="card-indicators">
            <span className="card-indicator-group"><BatteryIndicator millivolts={device.batt} percent={device.batteryPercent} /></span>
            <span className="card-indicator-group"><SignalIndicator rssi={device.rssi} snr={device.snr} ingestPath={device.ingestPath} /></span>
          </div>
          <div className="card-indicators card-indicators-row3">
            <HomeDistance>{distance}</HomeDistance>
            <LastSeen>{formatLastSeen(ageSeconds)}</LastSeen>
          </div>
        </div>
      </div>
      <div className="card-detail-reveal" aria-hidden={false}>
        <div className="card-detail-reveal-inner">
          <div className="card-detail">
            <div className="card-grid">
              <span className="label">Coordinates</span>
              <span className="value">
                <a className="card-coords card-coords-link" href={mapsUrl} target="_blank" rel="noopener noreferrer">
                  {formatMapCoordinates(device.lat, device.lon)}
                </a>
              </span>
              <span className="label">Dist From Hub</span><span className="value">{distance}</span>
              <span className="label">Last seen</span><span className="value">{formatAge(ageSeconds)}</span>
            </div>
            <div className="card-actions search-party-card-actions">
              <button className="btn-action btn-jump" type="button" onClick={onCentre}>↗ Centre</button>
            </div>
          </div>
        </div>
      </div>
    </article>
  );
}

function formatLastSeen(seconds: number) {
  if (seconds < 60) return `${seconds}s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ${seconds % 60}s`;
  return `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m`;
}

function formatAge(seconds: number) {
  if (seconds < 60) return `${seconds}s ago`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m ago`;
  return `${Math.floor(minutes / 60)}h ago`;
}

function haversine(lat1: number, lon1: number, lat2: number, lon2: number) {
  const toRad = (degrees: number) => degrees * Math.PI / 180;
  const radius = 6371000;
  const deltaLat = toRad(lat2 - lat1);
  const deltaLon = toRad(lon2 - lon1);
  const a = Math.sin(deltaLat / 2) ** 2 + Math.cos(toRad(lat1)) * Math.cos(toRad(lat2)) * Math.sin(deltaLon / 2) ** 2;
  return 2 * radius * Math.asin(Math.sqrt(a));
}

function formatDistance(metres: number) {
  return metres >= 2000 ? `${(metres / 1000).toFixed(1)} km` : `${Math.round(metres)} m`;
}
