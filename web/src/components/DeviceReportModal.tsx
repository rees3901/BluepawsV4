"use client";
import { createPortal } from "react-dom";
import type { DeviceReport, DeviceReportTableRow } from "@/lib/deviceReports";
import type { ReportExportFormat } from "@/lib/reportExport";

interface DeviceReportModalProps {
  deviceName: string;
  reports: Pick<DeviceReport, "rows" | "summary">[];
  entityLabel?: string;
  loading: boolean;
  error: string | null;
  onClose: () => void;
  onDownload: (format: ReportExportFormat) => void;
  exporting?: boolean;
}

export function DeviceReportModal({ deviceName, reports, loading, error, onClose, onDownload, exporting = false, entityLabel = "collar" }: DeviceReportModalProps) {
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
                    <th scope="col">Field</th>
                    <th scope="col">Data</th>
                    <th scope="col">Description</th>
                    <th scope="col">What this means</th>
                  </tr>
                </thead>
                <tbody>
                  {latest.rows.filter(row => !row.advanced).map((row) => (
                    <tr key={row.field}>
                      <th scope="row">{row.field}</th>
                      <td>{row.data}</td>
                      <td>{row.description}</td>
                      <td>{row.meaning}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>

            {latest.rows.some(row => row.advanced) && <details className="previous-reports report-diagnostics">
              <summary>Advanced packet diagnostics</summary>
              <ReportDiagnosticsTable rows={latest.rows.filter(row => row.advanced)} />
            </details>}

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

        <p className="report-summary">{entityLabel === "hub" ? "Exports the latest hub snapshot only, not a history archive." : `Preview: ${reports.length} recent report(s). Downloads retrieve up to the latest 1,000 accepted reports.`} JSON includes readings, timestamps and packet diagnostics. CSV contains one data row per report, without repeated advice. Downloads contain location data; share carefully.</p>
        <div className="modal-actions report-export-actions" aria-busy={exporting}>
          <button className="btn-primary" type="button" onClick={() => onDownload("json")} disabled={loading || exporting || reports.length === 0}>{exporting ? "Preparing download…" : "Download JSON"}</button>
          <button className="btn-secondary" type="button" onClick={() => onDownload("csv")} disabled={loading || exporting || reports.length === 0}>Data CSV</button>
          <button className="btn-secondary" type="button" onClick={onClose}>Close</button>
        </div>
      </div>
    </div>,
    document.body
  );
}

function ReportDiagnosticsTable({rows}: {rows: DeviceReportTableRow[]}) {
  return <div className="report-table-wrap"><table className="report-table" aria-label="Advanced packet diagnostics">
    <thead><tr>{["Field","Data","Description","What this means"].map(label => <th key={label} scope="col">{label}</th>)}</tr></thead>
    <tbody>{rows.map(row => <tr key={row.field}><th scope="row">{row.field}</th><td>{row.data}</td><td>{row.description}</td><td>{row.meaning}</td></tr>)}</tbody>
  </table></div>;
}
