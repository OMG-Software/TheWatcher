import React from 'react';
import { fetchAgentHistory } from '../api';
import type { StatusHistoryRow } from '../models';
import { Modal } from './Modal';

export function StatusHistoryModal({
  agentId,
  agentName,
  onClose,
}: {
  agentId: string;
  agentName: string;
  onClose: () => void;
}) {
  const [rows, setRows] = React.useState<StatusHistoryRow[] | null>(null);
  const [error, setError] = React.useState<string | null>(null);

  React.useEffect(() => {
    let cancelled = false;
    void fetchAgentHistory(agentId, 200)
      .then((data) => { if (!cancelled) setRows(data); })
      .catch((err) => { if (!cancelled) setError(err instanceof Error ? err.message : 'Failed to load status history'); });
    return () => { cancelled = true; };
  }, [agentId]);

  return (
    <Modal className="status-history-modal" onClose={onClose} title={`Status History — ${agentName}`}>
      <div className="status-history-body">
        {error ? <div className="banner error">{error}</div> : null}
        {!error && rows === null ? <div className="banner">Loading status history…</div> : null}
        {rows !== null && rows.length === 0 ? <div className="banner">No status history recorded for this agent.</div> : null}
        {rows !== null && rows.length > 0 ? (
          <table className="history-table">
            <thead>
              <tr>
                <th>Timestamp</th>
                <th>Indicator</th>
                <th colSpan={3}>Change</th>
                <th>Message</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((row) => (
                <tr key={row.id}>
                  <td className="history-ts">{new Date(row.created_at).toLocaleString()}</td>
                  <td>{row.indicator}</td>
                  <td><span className={`dot ${row.old_status} small-dot`} /></td>
                  <td>→</td>
                  <td><span className={`dot ${row.new_status} small-dot`} /></td>
                  <td className="history-msg">{row.message}</td>
                </tr>
              ))}
            </tbody>
          </table>
        ) : null}
      </div>
    </Modal>
  );
}

