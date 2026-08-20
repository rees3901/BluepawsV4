/* eslint-disable @next/next/no-img-element -- Tiny pre-sized emoji artwork is intentionally served directly from the picker CDN. */
import { BatteryIndicator, BleProximity, HomeDistance, LastSeen, SignalIndicator } from "@/components/Indicators";
import { emojiImageUrl } from "@/lib/emoji";
import { formatMapCoordinates, googleMapsUrl } from "@/lib/mapLocation";
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
  dragging: boolean;
  dragOver: boolean;
  followed: boolean;
  trailVisible: boolean;
  portableMode: boolean;
  distance: string;
  ageSeconds: number;
  onExpand: () => void;
  onAction: (action: DeviceAction) => void;
  onDragStart: () => void;
  onDragOver: () => void;
  onDrop: () => void;
  onDragEnd: () => void;
  onPinTop: () => void;
  onAvatarEdit?: () => void;
}

export function DeviceCard(props: DeviceCardProps) {
  const { device, avatar, expanded, dragging, dragOver, followed, trailVisible, portableMode, distance, ageSeconds, onExpand, onAction, onDragStart, onDragOver, onDrop, onDragEnd, onPinTop, onAvatarEdit } = props;
  const status = STATUS[device.status.toLowerCase() as keyof typeof STATUS] ?? STATUS.error;
  const profileLower = device.profile.toLowerCase();
  const profileClass = `profile-${profileLower.replace("save", "").replaceAll(" ", "-")}`;
  const profileLabel = profileLower === "powersave" ? "💤 PowerSave" : device.profile;
  const lastSeen = formatLastSeen(ageSeconds);

  return (
    <article
      className={`device-card${ageSeconds > 600 ? " stale" : ""}${expanded ? " expanded" : ""}${dragging ? " dragging" : ""}${dragOver ? " drag-over" : ""}`}
      onDragOver={(event) => {
        event.preventDefault();
        onDragOver();
      }}
      onDrop={(event) => {
        event.preventDefault();
        onDrop();
      }}
    >
      <div className="card-summary" onClick={onExpand}>
        <button
          type="button"
          className="card-reorder-handle"
          title={`Drag to reorder ${device.name}`}
          aria-label={`Drag to reorder ${device.name}`}
          draggable
          onClick={(event) => { event.stopPropagation(); }}
          onDragStart={(event) => {
            event.stopPropagation();
            event.dataTransfer.effectAllowed = "move";
            event.dataTransfer.setData("text/plain", String(device.id));
            onDragStart();
          }}
          onDragEnd={(event) => {
            event.stopPropagation();
            onDragEnd();
          }}
        >
          <span aria-hidden="true">⋮⋮</span>
        </button>
        <div className="card-avatar-wrap">
          <div
            className={`card-avatar${avatar.kind === "photo" ? " has-photo" : ""}`}
            style={{ borderColor: avatar.color, backgroundImage: avatar.photoUrl ? `url(${JSON.stringify(avatar.photoUrl)})` : undefined }}
          >
            {avatar.kind === "photo" && avatar.photoUrl ? null : (
              <img className="avatar-emoji-image" src={emojiImageUrl(avatar.emoji)} alt={avatar.emoji} draggable={false} />
            )}
          </div>
          <button
            type="button"
            className="card-pin-button"
            title={`Pin ${device.name} to the top`}
            aria-label={`Pin ${device.name} to the top`}
            onClick={(event) => { event.stopPropagation(); onPinTop(); }}
          >
            <PinIcon />
          </button>
          {onAvatarEdit && (
            <button
              type="button"
              className="card-avatar-edit"
              title={`Customise ${device.name}'s marker`}
              aria-label={`Customise ${device.name}'s marker`}
              aria-hidden={!expanded}
              tabIndex={expanded ? 0 : -1}
              onClick={(event) => { event.stopPropagation(); onAvatarEdit(); }}
            >+</button>
          )}
        </div>
        <div className="card-identity">
          <div className="card-name-row">
            <span className="card-name">{device.name}</span>
            <span className={`card-status ${status.css}`}>{status.emoji} {status.label}</span>
            <span className={`card-profile ${profileClass}`}>{profileLabel}</span>
            {device.error !== "None" && <span className="error-badge">{device.error}</span>}
          </div>
          <div className="card-indicators">
            <span className="card-indicator-group"><BatteryIndicator millivolts={device.batt} percent={device.batteryPercent} /></span>
            <span className="card-indicator-group"><SignalIndicator rssi={device.rssi} snr={device.snr} ingestPath={device.ingestPath} /></span>
            {portableMode && <span className="card-indicator-group"><BleProximity rssi={device.rssi === null ? null : device.rssi + 28} /></span>}
          </div>
          <div className="card-indicators card-indicators-row3">
            <HomeDistance>{distance}</HomeDistance>
            <LastSeen>{lastSeen}</LastSeen>
          </div>
        </div>
        <span className="card-chevron">{expanded ? "▲" : "▼"}</span>
      </div>

      <div className="card-detail-reveal" aria-hidden={!expanded} inert={!expanded}>
        <div className="card-detail-reveal-inner">
          <div className="card-detail">
            <div className="card-grid">
              <span className="label">Coordinates</span>
              <span className="value">
                <a className="card-coords card-coords-link" href={googleMapsUrl(device.lat, device.lon)} target="_blank" rel="noopener noreferrer">
                  {formatMapCoordinates(device.lat, device.lon)}
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
        </div>
      </div>
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

function PinIcon() {
  return (
    <svg viewBox="0 0 24 24" aria-hidden="true" fill="currentColor">
      <path d="M14.9 3.6 20.4 9l-1.9 1.9-1.2-1.2-4.1 4.1.3 3.1-1 1-3.4-3.4-4.1 4.1-1.4-1.4 4.1-4.1-3.4-3.4 1-1 3.1.3 4.1-4.1-1.2-1.2 1.9-1.9 1.7 1.8Z" />
    </svg>
  );
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
