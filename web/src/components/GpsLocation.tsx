import { gpsAgeLabel, gpsAsAt, gpsDisplayClass, hideGpsAgeBadge, type GpsFreshness } from "@/lib/gpsFreshness";

export function GpsAgeBadge({ gps }: { gps: GpsFreshness }) {
  if (hideGpsAgeBadge(gps)) return null;
  return <span className={`gps-age-badge ${gps.className}`} title={`${gps.label}. ${gps.notice}`}>{gps.warning ? "⚠ " : ""}{gpsAgeLabel(gps)}</span>;
}

export function GpsAsAt({ gps, now }: { gps: GpsFreshness; now: number }) {
  return <><span className="label">GPS as at</span><span className={`value gps-as-at ${gpsDisplayClass(gps)}`} title={gps.notice}>
    {gps.fixAt === null ? gpsAsAt(gps, now) : <time dateTime={new Date(gps.fixAt).toISOString()}>{gpsAsAt(gps, now)}</time>}
  </span></>;
}
