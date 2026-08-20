"use client";

import dynamic from "next/dynamic";
import { useCallback, useEffect, useMemo, useState } from "react";
import { defaultDeviceAvatar } from "@/lib/defaultDeviceAvatar";
import { formatMapCoordinates, googleMapsUrl, mapLocationShareText } from "@/lib/mapLocation";
import type { SearchPartySnapshot } from "@/lib/searchParty";
import type { DeviceAction, DeviceAvatar, MapCommand, TelemetryDevice, TrailPoint } from "@/types/telemetry";

const TrackingMap = dynamic(() => import("@/components/TrackingMap"), {
  ssr: false,
  loading: () => <div className="map-loading">Loading search map…</div>,
});

const REFRESH_INTERVAL_MS = 10_000;

interface SearchPartyViewerProps {
  token: string;
  initialSnapshot: SearchPartySnapshot;
}

export function SearchPartyViewer({ token, initialSnapshot }: SearchPartyViewerProps) {
  const [snapshot, setSnapshot] = useState(initialSnapshot);
  const [lastRefresh, setLastRefresh] = useState<Date | null>(() => new Date());
  const [refreshError, setRefreshError] = useState<string | null>(null);
  const [mapCommand, setMapCommand] = useState<MapCommand | null>(null);

  const refresh = useCallback(async () => {
    try {
      const response = await fetch(`/api/search-party/${encodeURIComponent(token)}`, {
        cache: "no-store",
        headers: { accept: "application/json" },
      });
      const next = await response.json() as SearchPartySnapshot;
      setSnapshot(next);
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
      <TrackingMap
        devices={devices}
        avatars={avatars}
        sidebarOpen={false}
        followedId={null}
        trailIds={new Set<number>()}
        trailHistory={{} as Record<number, TrailPoint[]>}
        command={mapCommand}
        onAction={handleAction}
        readOnly
      />
      <aside className="search-party-panel" aria-label="Search-party map details">
        <span className="settings-eyebrow">Search party map</span>
        <h1>{snapshot.familyName}</h1>
        <p>Read-only helper view. Positions refresh every 10 seconds; collar commands and account settings are unavailable.</p>
        <dl className="search-party-meta">
          <div><dt>Expires</dt><dd>{expiresText}</dd></div>
          <div><dt>Last refresh</dt><dd>{refreshedText}</dd></div>
        </dl>
        <button className="btn-primary" type="button" onClick={() => { void refresh(); }}>Refresh now</button>
        {refreshError && <p className="settings-message error" role="alert">{refreshError}</p>}
        <div className="search-party-device-list">
          {devices.length === 0 ? (
            <p className="settings-copy">No pets have reported yet.</p>
          ) : devices.map((device) => (
            <SearchPartyDeviceRow
              key={device.id}
              device={device}
              avatar={avatars[device.id] ?? defaultDeviceAvatar(device.id)}
              onCentre={() => setMapCommand({ type: "jump", deviceId: device.id, nonce: Date.now() })}
            />
          ))}
        </div>
      </aside>
    </main>
  );
}

function SearchPartyDeviceRow({ device, avatar, onCentre }: { device: TelemetryDevice; avatar: DeviceAvatar; onCentre: () => void }) {
  const coordinates = formatMapCoordinates(device.lat, device.lon, 5);
  const mapsUrl = googleMapsUrl(device.lat, device.lon);
  const shareText = mapLocationShareText(device.lat, device.lon);

  async function copyCoordinates() {
    await navigator.clipboard.writeText(shareText);
  }

  async function shareCoordinates() {
    if (navigator.share) {
      await navigator.share({ title: `${device.name} location`, text: coordinates, url: mapsUrl });
      return;
    }
    await copyCoordinates();
  }

  return (
    <article className="search-party-device-row">
      <span className="card-avatar search-party-avatar" style={{ borderColor: avatar.color }} aria-hidden="true">
        {avatar.emoji}
      </span>
      <div>
        <strong>{device.name}</strong>
        <a href={mapsUrl} target="_blank" rel="noreferrer">{coordinates}</a>
        <small>{timeAgo(device.lastUpdate)} · {device.status} · {device.batteryPercent ?? Math.round(device.batt / 100)}%</small>
      </div>
      <button className="btn-secondary" type="button" onClick={onCentre}>Centre</button>
      <button className="btn-secondary" type="button" onClick={copyCoordinates}>Copy</button>
      <button className="btn-secondary" type="button" onClick={shareCoordinates}>Share</button>
    </article>
  );
}

function timeAgo(timestamp: number) {
  const seconds = Math.max(0, Math.floor((Date.now() - timestamp) / 1000));
  if (seconds < 60) return `${seconds}s ago`;
  const minutes = Math.floor(seconds / 60);
  if (minutes < 60) return `${minutes}m ago`;
  return `${Math.floor(minutes / 60)}h ago`;
}
