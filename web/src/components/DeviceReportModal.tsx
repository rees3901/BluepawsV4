"use client";
import { createPortal } from "react-dom";
import type { DeviceReport } from "@/lib/deviceReports";

interface DeviceReportModalProps {
  deviceName: string;
  reports: Pick<DeviceReport, "rows" | "summary">[];
  entityLabel?: string;
  loading: boolean;
  error: string | null;
  onClose: () => void;
  onDownload: () => void;
}

export function DeviceReportModal({ deviceName, reports, loading, error, onClose, onDownload, entityLabel = "collar" }: DeviceReportModalProps) {
  const latest = reports[0] ?? null;

  // The sidebar animates/transforms: fixed descendants otherwise use its bounds.
  // Both hub and collar reports belong to the viewport, not to their card.
  if (typeof document === "undefined") return null;
  return createPortal(
    <div className="modal" role="dialog" aria-modal="true" aria-labelledby="device-report-title">
      <div className="modal-content device-report-modal">
        <div className="modal-header">
          <div>
            <span className="report-eyebrow">Latest {entityLabel} report</span>
            <h2 id="device-report-title">{deviceName}</h2>
          </div>
          <button className="modal-close-btn" type="button" aria-label="Close report log" onClick={onClose}>×</button>
        </div>

        {loading && <p className="report-summary">Loading the latest accepted reports…</p>}
        {error && <p className="settings-message error" role="alert">{error}</p>}
        {!loading && !error && !latest && (
          <p className="report-summary">No accepted reports are available for this device yet.</p>
        )}
        {latest && (
          <>
            <p className="report-summary">{latest.summary}</p>
            <div className="report-table-wrap">
              <table className="report-table">
                <thead>
                  <tr>
                    <th>Field</th>
                    <th>Data</th>
                    <th>What this means</th>
                  </tr>
                </thead>
                <tbody>
                  {latest.rows.map((row) => (
                    <tr key={row.field}>
                      <th scope="row">{row.field}</th>
                      <td>{row.data}</td>
                      <td>{row.description}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>

            {reports.length > 1 && (
              <details className="previous-reports">
                <summary>Previous accepted reports ({reports.length - 1})</summary>
                <ol>
                  {reports.slice(1).map((report, index) => (
                    <li key={index}>
                      {report.summary}
                    </li>
                  ))}
                </ol>
              </details>
            )}
          </>
        )}

        <div className="modal-actions">
          <button className="btn-primary" type="button" onClick={onDownload} disabled={reports.length === 0}>Download CSV</button>
          <button className="btn-secondary" type="button" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>,
    document.body
  );
}
