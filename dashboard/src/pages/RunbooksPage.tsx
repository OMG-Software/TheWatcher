import React from 'react';
import { Plus, Trash2 } from 'lucide-react';
import { createRunbook, deleteRunbook, fetchIndicatorKinds, fetchRunbooks } from '../api';
import { ActionError } from '../components/ActionError';
import { ActionButton } from '../components/ActionButton';
import { LoadFailed } from '../components/LoadFailed';
import { StatusDot } from '../components/StatusDot';
import { usePolling } from '../hooks/usePolling';
import type { RunAction } from '../hooks/useRunAction';
import type { DashboardAgent } from '../models';
import { safeRunbookUrl } from '../uiLogic';

/**
 * Runbook links. An alert on an agent gets the most specific link: the agent's
 * own runbook before a global one, and within each the exact indicator, then
 * its kind ("disk" for "disk:/"), then "*". Only global admins add global ones.
 */
export function RunbooksPage({
  agents = [],
  global = true,
  runAction,
}: {
  agents?: DashboardAgent[];
  global?: boolean;
  runAction: RunAction;
}) {
  const [agentId, setAgentId] = React.useState('');
  const [indicator, setIndicator] = React.useState('*');
  const [status, setStatus] = React.useState<'yellow' | 'amber' | 'red'>('red');
  const [url, setUrl] = React.useState('');
  const [notes, setNotes] = React.useState('');

  const { data, error, loading, refresh } = usePolling(fetchRunbooks);
  const runbooks = data ?? [];
  const kinds = usePolling(fetchIndicatorKinds).data ?? [];
  // Operators outside the global roles may only write agent-scoped runbooks.
  const selectedAgent = agentId || (global ? '' : (agents[0]?.id ?? ''));
  const agentName = (id: string) => (id === '' ? 'All agents' : (agents.find((a) => a.id === id)?.name || id));

  return (
    <main className="table-wrap management-wrap">
      <div className="management-header"><h1>Runbooks</h1></div>
      <form
        className="settings-form"
        onSubmit={(event) => {
          event.preventDefault();
          void runAction('runbook:create', () => createRunbook(indicator, status, url, notes, selectedAgent).then(() => undefined), { inline: true }).then(async (saved) => {
            if (!saved) return; // keep the form as typed
            setUrl('');
            setNotes('');
            setIndicator('*');
            await refresh();
          });
        }}
      >
        <div className="form-grid">
          <label>
            Agent
            <select value={selectedAgent} onChange={(e) => setAgentId(e.target.value)}>
              {global ? <option value="">All agents</option> : null}
              {agents.map((a) => (
                <option key={a.id} value={a.id}>{a.name || a.id}</option>
              ))}
            </select>
          </label>
          <label>
            Indicator (name, kind such as disk, or * for any)
            <input
              list="runbook-indicators"
              placeholder="cpu"
              value={indicator}
              onChange={(e) => setIndicator(e.target.value)}
            />
            <datalist id="runbook-indicators">
              <option value="*" />
              {kinds.map((kind) => <option key={kind.name} value={kind.name} />)}
            </datalist>
          </label>
          <label>
            Status
            <select value={status} onChange={(e) => setStatus(e.target.value as 'yellow' | 'amber' | 'red')}>
              <option value="yellow">Yellow</option>
              <option value="amber">Amber</option>
              <option value="red">Red</option>
            </select>
          </label>
          <label>
            Runbook URL
            <input
              placeholder="https://wiki.example.com/runbook"
              type="url"
              value={url}
              onChange={(e) => setUrl(e.target.value)}
            />
          </label>
          <label>
            Notes
            <input
              placeholder="Optional notes"
              value={notes}
              onChange={(e) => setNotes(e.target.value)}
            />
          </label>
        </div>
        <ActionError actionKey="runbook:create" />
        <div className="button-row">
          <button className="text-button" type="submit" disabled={!url}>
            <Plus size={14} /> Add Runbook
          </button>
        </div>
      </form>
      {error ? <LoadFailed what="runbooks" message={error} stale={data !== null} onRetry={() => void refresh()} /> : null}
      <div className="table-outer">
        <table className="agent-table">
          <thead><tr><th>Agent</th><th>Indicator</th><th>Status</th><th>URL</th><th>Notes</th><th>Created By</th><th>Actions</th></tr></thead>
          <tbody>
            {loading ? (
              <tr><td colSpan={7} className="uptime">Loading runbooks...</td></tr>
            ) : runbooks.length === 0 && !error ? (
              <tr><td colSpan={7} className="uptime">No runbooks configured.</td></tr>
            ) : runbooks.map((rb) => (
              <tr key={rb.runbook_id}>
                <td>{agentName(rb.agent_id)}</td>
                <td>{rb.indicator}</td>
                <td><StatusDot color={rb.status} label={rb.status} /></td>
                <td>
                  {safeRunbookUrl(rb.url) ? <a href={safeRunbookUrl(rb.url) as string} target="_blank" rel="noopener noreferrer">{rb.url}</a> : rb.url}
                </td>
                <td>{rb.notes}</td>
                <td className="uptime">{rb.created_by}</td>
                <td>
                  <ActionButton
                    busy={false}
                    danger
                    icon={<Trash2 size={14} />}
                    label="Delete"
                    onClick={() => void runAction(`runbook:delete:${rb.runbook_id}`, () => deleteRunbook(rb.runbook_id)).then((deleted) => deleted ? refresh() : undefined)}
                  />
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </main>
  );
}

