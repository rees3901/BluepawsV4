import { BatteryIndicator, BleProximity, HomeDistance, LastSeen, SignalIndicator } from "@/components/Indicators";
import type { DeviceAction, DeviceAvatar, TelemetryDevice } from "@/types/telemetry";

const STATUS = {
  home: { emoji: "🏠", label: "Home", css: "status-home" },
  out: { emoji: "🐾", label: "Out", css: "status-out" },
  lost: { emoji: "‼", label: "Lost", css: "status-lost" },
  error: { emoji: "❓", label: "Error", css: "status-error" },
};

interface DeviceCardProps {
  device: TelemetryDevice;
  avatar: DeviceAvatar;
  expanded: boolean;
  followed: boolean;
  trailVisible: boolean;
  portableMode: boolean;
  distance: string;
  ageSeconds: number;
  onExpand: () => void;
  onAction: (action: DeviceAction) => void;
}

export function DeviceCard(props: DeviceCardProps) {
  const { device, avatar, expanded, followed, trailVisible, portableMode, distance, ageSeconds, onExpand, onAction } = props;
  const status = STATUS[device.status.toLowerCase() as keyof typeof STATUS] ?? STATUS.error;
  const profileLower = device.profile.toLowerCase();
  const profileClass = `profile-${profileLower.replace("save", "").replaceAll(" ", "-")}`;
  const profileLabel = profileLower === "powersave" ? "💤 PowerSave" : device.profile;
  const lastSeen = formatLastSeen(ageSeconds);

  return (
    <article className={`device-card${ageSeconds > 600 ? " stale" : ""}${expanded ? " expanded" : ""}`}>
      <div className="card-summary" onClick={onExpand}>
        <div className="card-avatar" style={{ borderColor: avatar.color }}>{avatar.emoji}</div>
        <div className="card-identity">
          <div className="card-name-row">
            <span className="card-name">{device.name}</span>
            <span className={`card-status ${status.css}`}>{status.emoji} {status.label}</span>
            <span className={`card-profile ${profileClass}`}>{profileLabel}</span>
            {device.error !== "None" && <span className="error-badge">{device.error}</span>}
          </div>
          <div className="card-indicators">
            <span className="card-indicator-group"><BatteryIndicator millivolts={device.batt} /></span>
            <span className="card-indicator-group"><SignalIndicator rssi={device.rssi} snr={device.snr} /></span>
            {portableMode && <span className="card-indicator-group"><BleProximity rssi={device.rssi + 28} /></span>}
          </div>
          <div className="card-indicators card-indicators-row3">
            <HomeDistance>{distance}</HomeDistance>
            <LastSeen>{lastSeen}</LastSeen>
          </div>
        </div>
        <span className="card-chevron">{expanded ? "▲" : "▼"}</span>
      </div>

      {expanded && (
        <div className="card-detail">
          <div className="card-grid">
            <span className="label">Coordinates</span>
            <span className="value">
              <a className="card-coords card-coords-link" href={`https://www.google.com/maps?q=${device.lat.toFixed(6)},${device.lon.toFixed(6)}`} target="_blank" rel="noopener noreferrer">
                {device.lat.toFixed(5)}, {device.lon.toFixed(5)}
              </a>
            </span>
            <span className="label">Power Profile</span><span className="value">{device.profile}</span>
            <span className="label">Dist From Hub</span><span className="value">{distance}</span>
            <span className="label">Last seen</span><span className="value">{formatAge(ageSeconds)}</span>
          </div>
          <ActionButtons followed={followed} trailVisible={trailVisible} onAction={onAction} />
          <div className="log-btn-row">
            <button className="btn-device-log btn-secondary">Message Log</button>
            <button className="btn-log-export" title="Export log as CSV" aria-label="Export device log">
              <DownloadIcon />
            </button>
          </div>
        </div>
      )}
    </article>
  );
}

export function ActionButtons({ followed, trailVisible, onAction }: { followed: boolean; trailVisible: boolean; onAction: (action: DeviceAction) => void }) {
  return (
    <div className="card-actions" onClick={(event) => event.stopPropagation()}>
      <button className="btn-action btn-jump" onClick={() => onAction("jump")} title="Jump to location">↗ Jump To</button>
      <button className={`btn-action btn-follow${followed ? " active" : ""}`} onClick={() => onAction("follow")} title="Auto-follow on map">● {followed ? "Following" : "Follow"}</button>
      <button className={`btn-action btn-trail${trailVisible ? " active" : ""}`} onClick={() => onAction("trail")} title="Toggle breadcrumb trail">⌁ Trail</button>
      <button className="btn-action btn-find" onClick={() => onAction("find")} title="Find Alert — trigger buzzer + LED">♟ Find Alert</button>
      <button className="btn-action btn-cmd" onClick={() => onAction("command")} title="Command & Control">⌘ Cmd</button>
    </div>
  );
}

export function DownloadIcon() {
  return <svg className="icon-download" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"><path d="M12 4v12m0 0l-4-4m4 4l4-4" /><path d="M5 20h14" /></svg>;
}

function formatLastSeen(seconds: number) {
  if (seconds < 60) return `${seconds}s`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ${seconds % 60}s`;
  return `${Math.floor(seconds / 3600)}h ${Math.floor((seconds % 3600) / 60)}m`;
}

function formatAge(seconds: number) {
  if (seconds < 5) return "just now";
  if (seconds < 60) return `${seconds}s ago`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}m ago`;
  return `${Math.floor(seconds / 3600)}h ago`;
}
