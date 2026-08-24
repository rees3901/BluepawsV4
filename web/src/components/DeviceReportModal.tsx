import type { DeviceReport } from "@/lib/deviceReports";

interface DeviceReportModalProps {
  deviceName: string;
  reports: DeviceReport[];
  loading: boolean;
  error: string | null;
  onClose: () => void;
  onDownload: () => void;
}

export function DeviceReportModal({ deviceName, reports, loading, error, onClose, onDownload }: DeviceReportModalProps) {
  const latest = reports[0] ?? null;

  return (
    <div className="modal" role="dialog" aria-modal="true" aria-labelledby="device-report-title">
      <div className="modal-content device-report-modal">
        <div className="modal-header">
          <div>
            <span className="report-eyebrow">Latest collar report</span>
            <h2 id="device-report-title">{deviceName}</h2>
          </div>
          <button className="modal-close-btn" type="button" aria-label="Close report log" onClick={onClose}>×</button>
        </div>

        {loading && <p className="report-summary">Loading the latest accepted reports…</p>}
        {error && <p className="settings-message error" role="alert">{error}</p>}
        {!loading && !error && !latest && (
          <p className="report-summary">No accepted reports are available for this pet yet.</p>
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
                  {reports.slice(1).map((report) => (
                    <li key={`${report.observation.id}-${report.observation.msg_seq_id}`}>
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
    </div>
  );
}
