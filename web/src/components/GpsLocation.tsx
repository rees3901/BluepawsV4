import { gpsAsAt, type GpsFreshness } from "@/lib/gpsFreshness";

export function GpsAgeBadge({ gps }: { gps: GpsFreshness }) {
  return <span className={`gps-age-badge ${gps.className}`} title={gps.notice}>{gps.warning ? "⚠ " : ""}{gps.label}</span>;
}

export function GpsAsAt({ gps, now }: { gps: GpsFreshness; now: number }) {
  return <><span className="label">GPS as at</span><span className={`value gps-as-at ${gps.className}`} title={gps.notice}>
    {gps.fixAt === null ? gpsAsAt(gps, now) : <time dateTime={new Date(gps.fixAt).toISOString()}>{gpsAsAt(gps, now)}</time>}
  </span></>;
}
