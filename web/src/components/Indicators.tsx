import type { ReactNode } from "react";
import { transportPresentation } from "@/lib/transportPath";
import type { IngestPath } from "@/types/telemetry";

export function signalQuality(rssi: number, snr: number) {
  const rssiScore = rssi > -80 ? 4 : rssi > -100 ? 3 : rssi > -110 ? 2 : rssi > -120 ? 1 : 0;
  const snrScore = snr > 7 ? 4 : snr > 5 ? 3 : snr > 0 ? 2 : snr > -5 ? 1 : 0;
  const combined = rssiScore * 0.6 + snrScore * 0.4;
  if (combined >= 3.5) return { level: 5, label: "Excellent", color: "#22c55e" };
  if (combined >= 2.5) return { level: 4, label: "Good", color: "#84cc16" };
  if (combined >= 1.5) return { level: 3, label: "Average", color: "#f59e0b" };
  if (combined >= 0.8) return { level: 2, label: "Poor", color: "#f97316" };
  return { level: 1, label: "Very Poor", color: "#ef4444" };
}

function batteryLevel(millivolts: number) {
  const volts = millivolts / 1000;
  if (volts >= 4.1) return { level: 5, label: "Full", color: "#22c55e" };
  if (volts >= 3.95) return { level: 4, label: "Very Good", color: "#84cc16" };
  if (volts >= 3.8) return { level: 3, label: "Medium", color: "#f59e0b" };
  if (volts >= 3.65) return { level: 2, label: "Low", color: "#f97316" };
  return { level: 1, label: "Nearly Empty", color: "#ef4444" };
}

function batteryPercentLevel(percent: number) {
  if (percent >= 90) return { level: 5, label: "Full", color: "#22c55e" };
  if (percent >= 70) return { level: 4, label: "Very Good", color: "#84cc16" };
  if (percent >= 40) return { level: 3, label: "Medium", color: "#f59e0b" };
  if (percent >= 15) return { level: 2, label: "Low", color: "#f97316" };
  return { level: 1, label: "Nearly Empty", color: "#ef4444" };
}

export function batteryPresentation(millivolts: number | null, percent?: number | null) {
  const hasPercent = percent !== undefined && percent !== null;
  const quality = hasPercent ? batteryPercentLevel(percent) : millivolts === null ? { level: 0, label: "No data", color: "#607d8b" } : batteryLevel(millivolts);
  const measurement = hasPercent ? `${percent}%` : millivolts === null ? "Battery not reported" : `${(millivolts / 1000).toFixed(2)} V`;
  return { ...quality, measurement };
}

export function SignalIndicator({ rssi, snr, ingestPath }: { rssi: number | null; snr: number | null; ingestPath: IngestPath | null }) {
  const transport = transportPresentation(ingestPath);
  if (rssi === null || snr === null) {
    return (
      <span className="signal-indicator" title={`${transport.label}; radio signal was not included in this report`}>
        <AntennaIcon />
        {[1, 2, 3, 4, 5].map((bar) => <span key={bar} className="sig-bar" style={{ height: 4 + bar * 3 }} />)}
        <span className="sig-label">Not reported</span>
        <TransportBadge ingestPath={ingestPath} />
      </span>
    );
  }

  const signal = signalQuality(rssi, snr);
  return (
    <span className="signal-indicator" title={`${transport.label}; RSSI: ${rssi} dBm / SNR: ${snr} dB — ${signal.label}`}>
      <AntennaIcon />
      {[1, 2, 3, 4, 5].map((bar) => (
        <span key={bar} className={`sig-bar${bar <= signal.level ? " filled" : ""}`} style={{ height: 4 + bar * 3, background: bar <= signal.level ? signal.color : undefined }} />
      ))}
      <span className="sig-label" style={{ color: signal.color }}>{signal.label}</span>
      <TransportBadge ingestPath={ingestPath} />
    </span>
  );
}

// Wi-Fi is not LoRa: show its reported RSSI without inventing an SNR or RF badge.
export function WifiIndicator({ rssi, contactLost = false }: { rssi: number | null; contactLost?: boolean }) {
  if (contactLost) rssi = null;
  // Wi-Fi RSSI thresholds, not the LoRa RSSI/SNR scoring model.
  const level = rssi === null ? 0 : rssi >= -50 ? 5 : rssi >= -60 ? 4 : rssi >= -70 ? 3 : rssi >= -80 ? 2 : 1;
  const label = contactLost ? "No contact" : ["No Wi-Fi", "Very poor", "Poor", "Average", "Good", "Excellent"][level];
  const color = ["#607d8b", "#ef4444", "#f97316", "#f59e0b", "#84cc16", "#22c55e"][level];
  return <span className="signal-indicator" title={contactLost ? "Hub report overdue; current Wi-Fi connection is unknown" : `Wi-Fi ${rssi === null ? "not connected" : `${rssi} dBm`} — ${label}`}>
    <AntennaIcon />
    {[1,2,3,4,5].map(bar => <span key={bar} className={`sig-bar${bar <= level ? " filled" : ""}`} style={{height:4+bar*3, backgroundColor:bar <= level ? color : undefined}} />)}
    <span className="sig-label" style={{color}}>{label}</span>
    <span className="transport-badge" title="Home Hub Wi-Fi uplink" aria-label="Wi-Fi">Wi-Fi</span>
  </span>;
}

function TransportBadge({ ingestPath }: { ingestPath: IngestPath | null }) {
  const transport = transportPresentation(ingestPath);
  return (
    <span
      className={`transport-badge ${transport.cssClass}`}
      title={transport.label}
      role="img"
      aria-label={transport.label}
    >
      {transport.badge}
    </span>
  );
}

export function BatteryIndicator({ millivolts, percent }: { millivolts: number | null; percent?: number | null }) {
  const battery = batteryPresentation(millivolts, percent);
  return (
    <span className="battery-indicator" title={`${battery.measurement} — ${battery.label}`}>
      <svg className="indicator-icon icon-battery" viewBox="0 0 28 18" fill="none">
        <rect x="1" y="1" width="23" height="16" rx="3" stroke="#607d8b" strokeWidth="2" />
        <rect x="24" y="5.5" width="3" height="7" rx="1.2" fill="#607d8b" />
        {[0, 1, 2, 3, 4].map((bar) => <rect key={bar} x={4 + bar * 3.4} y="4.5" width="2.6" height="9" rx="0.6" fill={bar < battery.level ? battery.color : "#2f3e4e"} />)}
      </svg>
      <span className="sig-label" style={{ color: battery.color }}>{battery.label}</span>
    </span>
  );
}

export function HomeDistance({ children }: { children: ReactNode }) {
  return (
    <span className="card-indicator-group card-dist-group" title="Distance from home">
      <svg className="indicator-icon icon-home" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
        <path d="M3 12l9-9 9 9" /><path d="M5 10v10a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1V10" />
      </svg>
      <span className="card-dist-value">{children}</span>
    </span>
  );
}

export function LastSeen({ children }: { children: ReactNode }) {
  return (
    <span className="card-indicator-group card-lastseen-group" title="Last seen">
      <svg className="indicator-icon icon-stopwatch" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round">
        <circle cx="12" cy="13" r="8" /><line x1="12" y1="9" x2="12" y2="13" />
        <line x1="9" y1="1" x2="15" y2="1" /><line x1="12" y1="1" x2="12" y2="5" />
      </svg>
      <span className="card-lastseen-value">{children}</span>
    </span>
  );
}

export function BleProximity({ rssi }: { rssi: number | null }) {
  if (rssi === null) {
    return (
      <span className="ble-proximity" title="BLE proximity was not included in this report">
        {[1, 2, 3, 4].map((bar) => <span key={bar} className="ble-bar" style={{ height: 4 + bar * 3 }} />)}
        <span className="ble-proximity-label">Not reported</span>
      </span>
    );
  }

  const level = rssi >= -50 ? 4 : rssi >= -65 ? 3 : rssi >= -80 ? 2 : 1;
  const label = level === 4 ? "Very Close" : level === 3 ? "Close" : level === 2 ? "Medium" : "Far";
  return (
    <span className="ble-proximity" title={`BLE RSSI: ${rssi} dBm — ${label}`}>
      {[1, 2, 3, 4].map((bar) => <span key={bar} className={`ble-bar${bar <= level ? " filled" : ""}`} style={{ height: 4 + bar * 3 }} />)}
      <span className="ble-proximity-label">{label}</span>
    </span>
  );
}

function AntennaIcon() {
  return (
    <svg className="indicator-icon icon-antenna" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round">
      <line x1="12" y1="24" x2="12" y2="10" /><line x1="12" y1="10" x2="3" y2="2" />
      <line x1="12" y1="10" x2="21" y2="2" /><line x1="3" y1="2" x2="21" y2="2" />
    </svg>
  );
}
