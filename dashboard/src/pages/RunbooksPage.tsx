import React from 'react';
import { Plus, Trash2 } from 'lucide-react';
import { createRunbook, deleteRunbook, fetchRunbooks } from '../api';
import { ActionError } from '../components/ActionError';
import { ActionButton } from '../components/ActionButton';
import { LoadFailed } from '../components/LoadFailed';
import { StatusDot } from '../components/StatusDot';
import { usePolling } from '../hooks/usePolling';
import type { RunAction } from '../hooks/useRunAction';

export function RunbooksPage({
  runAction,
}: {
  runAction: RunAction;
}) {
  const [indicator, setIndicator] = React.useState('*');
  const [status, setStatus] = React.useState<'yellow' | 'amber' | 'red'>('red');
  const [url, setUrl] = React.useState('');
  const [notes, setNotes] = React.useState('');

  const { data, error, refresh } = usePolling(fetchRunbooks);
  const runbooks = data ?? [];

  return (
    <main className="table-wrap management-wrap">
      <div className="management-header"><h1>Runbooks</h1></div>
      <form
        className="settings-form"
        onSubmit={(event) => {
          event.preventDefault();
          void runAction('runbook:create', () => createRunbook(indicator, status, url, notes).then(() => undefined), { inline: true }).then(async (saved) => {
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
            Indicator (exact name or * for any)
            <input
              placeholder="cpu"
              value={indicator}
              onChange={(e) => setIndicator(e.target.value)}
            />
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
          <thead><tr><th>Indicator</th><th>Status</th><th>URL</th><th>Notes</th><th>Created By</th><th>Actions</th></tr></thead>
          <tbody>
            {runbooks.length === 0 && !error ? (
              <tr><td colSpan={6} className="uptime">No runbooks configured.</td></tr>
            ) : runbooks.map((rb) => (
              <tr key={rb.runbook_id}>
                <td>{rb.indicator}</td>
                <td><StatusDot color={rb.status} label={rb.status} /></td>
                <td><a href={rb.url} target="_blank" rel="noopener noreferrer">{rb.url}</a></td>
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

