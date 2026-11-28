import React from 'react';
import { Archive, BookOpen, Check, CheckSquare, Square } from 'lucide-react';
import { acknowledgeAlert, archiveAlert, bulkAcknowledgeAlerts, bulkArchiveAlerts, fetchAlerts } from '../api';
import { AcknowledgeModal } from '../components/AcknowledgeModal';
import { ActionButton } from '../components/ActionButton';
import { AgentIdentity } from '../components/AgentIdentity';
import { LoadFailed } from '../components/LoadFailed';
import { RunbookModal } from '../components/RunbookModal';
import { StatusDot } from '../components/StatusDot';
import { usePolling } from '../hooks/usePolling';
import type { RunAction } from '../hooks/useRunAction';
import type { DashboardAgent } from '../models';
import { toDisplayAlerts } from '../status';
import { colorLabel } from '../uiLogic';

const ARCHIVED_POLL_MS = 5000;

export function AlertsPage({
  agents,
  alerts,
  isBusy,
  operator,
  runAction: runPageAction,
}: {
  agents: DashboardAgent[];
  alerts: ReturnType<typeof toDisplayAlerts>;
  isBusy: (key: string) => boolean;
  operator: boolean;
  runAction: RunAction;
}) {
  const [selected, setSelected] = React.useState<Set<number>>(new Set());
  const [showArchived, setShowArchived] = React.useState(false);
  const [agentFilter, setAgentFilter] = React.useState('');
  const [statusFilter, setStatusFilter] = React.useState('');
  const [ackTarget, setAckTarget] = React.useState<number | 'bulk' | null>(null);
  const [runbookTarget, setRunbookTarget] = React.useState<{ agentName: string; markdown: string } | null>(null);

  // Archived alerts poll on their own timer, and only while they are shown.
  // (They used to re-fetch whenever the live list changed, i.e. on every poll.)
  const archived = usePolling(React.useCallback((signal: AbortSignal) => fetchAlerts(true, signal), []), { intervalMs: ARCHIVED_POLL_MS, enabled: showArchived });
  const reloadArchived = archived.reload;
  const archivedAlerts = React.useMemo(() => toDisplayAlerts(archived.data ?? [], agents), [archived.data, agents]);
  // Until the first archived load succeeds there is no archived list: the table must not read as "no alerts".
  const archivedUnavailable = showArchived && archived.data === null;

  // A successful action reloads the archived list too (it polls only every few seconds). `reload` drops
  // an archived request that started before the action, so its old result cannot be shown afterwards.
  const runAction = React.useCallback<RunAction>(
    async (key, action, options) => {
      const saved = await runPageAction(key, action, options);
      if (saved && showArchived) await reloadArchived();
      return saved;
    },
    [reloadArchived, runPageAction, showArchived],
  );

  const baseAlerts = showArchived ? archivedAlerts : alerts;
  const displayed = baseAlerts.filter((a) => {
    if (agentFilter && a.agentId !== agentFilter) return false;
    if (statusFilter && a.new_status !== statusFilter) return false;
    return true;
  });

  const displayedIds = displayed.map((a) => a.alert_id);
  const allSelected = displayedIds.length > 0 && displayedIds.every((id) => selected.has(id));

  const toggleSelect = (id: number) =>
    setSelected((prev) => { const n = new Set(prev); if (n.has(id)) n.delete(id); else n.add(id); return n; });
  const toggleSelectAll = () =>
    setSelected(allSelected ? new Set() : new Set(displayedIds));

  const afterAction = () => setSelected(new Set());

  const ackKey = ackTarget === 'bulk' ? 'bulk:ack' : `alert:${ackTarget}:ack`;

  // The dialog closes, and the selection clears, only when the server accepted the change.
  const handleAckSubmit = async (note: string, andArchive: boolean) => {
    let saved = false;
    if (ackTarget === 'bulk') {
      const ids = [...selected];
      saved = await runAction(ackKey, async () => {
        await bulkAcknowledgeAlerts(ids, note);
        if (andArchive) await bulkArchiveAlerts(ids);
      }, { inline: true });
    } else if (ackTarget !== null) {
      const id = ackTarget;
      saved = await runAction(ackKey, async () => {
        await acknowledgeAlert(id, note);
        if (andArchive) await archiveAlert(id);
      }, { inline: true });
    }
    if (saved) {
      setAckTarget(null);
      afterAction();
    }
  };

  const uniqueAgents = [...new Map(alerts.map((a) => [a.agentId, a.agentName])).entries()];
  const selectedCount = selected.size;

  return (
    <main className="table-wrap management-wrap">
      {ackTarget !== null ? (
        <AcknowledgeModal busy={isBusy(ackKey)} bulk={ackTarget === 'bulk'} errorKey={ackKey} onClose={() => setAckTarget(null)} onSubmit={handleAckSubmit} />
      ) : null}
      {runbookTarget ? (
        <RunbookModal
          agentName={runbookTarget.agentName}
          markdown={runbookTarget.markdown}
          onClose={() => setRunbookTarget(null)}
        />
      ) : null}
      <div className="management-header">
        <h1>Alerts</h1>
        <div className="header-controls">
          <select value={agentFilter} onChange={(e) => setAgentFilter(e.target.value)}>
            <option value="">All agents</option>
            {uniqueAgents.map(([id, name]) => <option key={id} value={id}>{name}</option>)}
          </select>
          <select value={statusFilter} onChange={(e) => setStatusFilter(e.target.value)}>
            <option value="">All statuses</option>
            <option value="red">Critical</option>
            <option value="amber">Degraded</option>
            <option value="yellow">Warning</option>
            <option value="green">Healthy</option>
          </select>
          <label className="checkbox-label">
            <input type="checkbox" checked={showArchived} onChange={(e) => setShowArchived(e.target.checked)} />
            Show archived
          </label>
        </div>
      </div>
      {selectedCount > 0 && operator ? (
        <div className="bulk-action-bar">
          <span>{selectedCount} selected</span>
          <div className="button-row">
            <ActionButton busy={isBusy('bulk:ack')} icon={<Check size={14} />} label={`Acknowledge (${selectedCount})`} onClick={() => setAckTarget('bulk')} />
            <ActionButton busy={isBusy('bulk:archive')} danger icon={<Archive size={14} />} label={`Archive (${selectedCount})`} onClick={() => runAction('bulk:archive', async () => { await bulkArchiveAlerts([...selected]); afterAction(); })} />
          </div>
        </div>
      ) : null}
      {showArchived && archived.loading ? <div className="banner">Loading archived alerts...</div> : null}
      {showArchived && archived.error ? <LoadFailed what="archived alerts" message={archived.error} stale={!archivedUnavailable} onRetry={() => void archived.refresh()} /> : null}
      <div className="table-container">
        <table className="management-table">
          <thead>
            <tr>
              <th className="col-check">
                <button className="icon-button" onClick={toggleSelectAll} disabled={displayedIds.length === 0}>
                  {allSelected ? <CheckSquare size={14} /> : <Square size={14} />}
                </button>
              </th>
              <th>Agent</th><th>Indicator</th><th>State</th><th>Created</th><th>Message</th><th>Acknowledged</th><th>Actions</th>
            </tr>
          </thead>
          <tbody>{displayed.map((alert) => {
            const isAcknowledged = alert.acknowledged_at > 0;
            const isArchived = alert.deleted_at > 0;
            const agent = agents.find((candidate) => candidate.id === alert.agentId);
            const runbookMarkdown = alert.runbook_markdown || agent?.runbookMarkdown || '';
            return (
              <tr key={alert.alert_id} className={isArchived ? 'row-archived' : ''}>
                <td className="col-check">
                  <button className="icon-button" onClick={() => toggleSelect(alert.alert_id)}>
                    {selected.has(alert.alert_id) ? <CheckSquare size={14} /> : <Square size={14} />}
                  </button>
                </td>
                <td><AgentIdentity id={alert.agentId} name={alert.agentName} /></td>
                <td>{alert.indicator}</td>
                <td><StatusDot color={alert.new_status} label={colorLabel(alert.new_status)} acknowledged={isAcknowledged} /></td>
                <td className="uptime">{new Date(alert.created_at).toLocaleString()}</td>
                <td>
                  {alert.message}
                  {runbookMarkdown.trim() ? (
                    <button
                      className="runbook-link"
                      onClick={() => setRunbookTarget({ agentName: alert.agentName, markdown: runbookMarkdown })}
                      type="button"
                    >
                      <BookOpen size={12} /> Runbook
                    </button>
                  ) : null}
                </td>
                <td className="uptime">
                  {isAcknowledged ? (
                    <span>
                      {alert.acknowledged_by} &mdash; {new Date(alert.acknowledged_at).toLocaleString()}
                      {alert.note ? <em className="alert-note"> — {alert.note}</em> : null}
                    </span>
                  ) : alert.escalated_at > 0 ? (
                    <span className="escalated-badge">ESCALATED</span>
                  ) : null}
                </td>
                <td>
                  {!isArchived ? (
                    <div className="button-row">
                      {!isAcknowledged ? (
                        <>
                          <ActionButton busy={isBusy(`alert:${alert.alert_id}:ack`)} disabled={!operator} icon={<Check size={14} />} label="Acknowledge" onClick={() => setAckTarget(alert.alert_id)} />
                          <ActionButton busy={isBusy(`alert:${alert.alert_id}:ack-archive`)} disabled={!operator} icon={<Archive size={14} />} label="Acknowledge & Archive" onClick={() => runAction(`alert:${alert.alert_id}:ack-archive`, async () => { await acknowledgeAlert(alert.alert_id); await archiveAlert(alert.alert_id); })} />
                        </>
                      ) : (
                        <ActionButton busy={isBusy(`alert:${alert.alert_id}:archive`)} danger disabled={!operator} icon={<Archive size={14} />} label="Archive" onClick={() => runAction(`alert:${alert.alert_id}:archive`, () => archiveAlert(alert.alert_id))} />
                      )}
                    </div>
                  ) : <span className="uptime">Archived</span>}
                </td>
              </tr>
            );
          })}
            {archivedUnavailable ? (
              // While loading, the banner above already says so; only a failed load needs a row.
              archived.loading ? null : <tr><td colSpan={8} className="empty-row">Archived alerts are unavailable.</td></tr>
            ) : displayed.length === 0 ? (
              <tr><td colSpan={8} className="empty-row">{baseAlerts.length === 0 ? 'No alerts.' : 'No alerts match the filters.'}</td></tr>
            ) : null}
          </tbody>
        </table>
      </div>
    </main>
  );
}

