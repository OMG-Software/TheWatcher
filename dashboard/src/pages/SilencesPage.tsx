import React from 'react';
import { Plus, Trash2 } from 'lucide-react';
import { createSilence, deleteSilence, fetchIndicatorKinds } from '../api';
import { ActionError } from '../components/ActionError';
import { ActionButton } from '../components/ActionButton';
import { LoadFailed } from '../components/LoadFailed';
import { usePolling } from '../hooks/usePolling';
import type { RunAction } from '../hooks/useRunAction';
import type { DashboardAgent, SilenceRecord } from '../models';

export function SilencesPage({
  agents,
  global,
  isBusy,
  loadFailed = false,
  operator,
  runAction,
  silences,
}: {
  agents: DashboardAgent[];
  global: boolean;
  isBusy: (key: string) => boolean;
  loadFailed?: boolean;
  operator: boolean;
  runAction: RunAction;
  silences: SilenceRecord[];
}) {
  const [agentId, setAgentId] = React.useState('*');
  const [indicator, setIndicator] = React.useState('*');
  const [qualifier, setQualifier] = React.useState('');
  // The indicator names come from the server, which is what raises the alerts.
  const kinds = usePolling(fetchIndicatorKinds);
  const kindList = kinds.data ?? [];
  const selectedKind = kindList.find((kind) => kind.name === indicator);
  const [reason, setReason] = React.useState('');
  const [hours, setHours] = React.useState(1);

  React.useEffect(() => {
    if (!global && agentId === '*' && agents.length > 0) setAgentId(agents[0].id);
  }, [agentId, agents, global]);

  return (
    <main className="table-wrap management-wrap">
      <div className="management-header"><h1>Silence Rules</h1></div>
      {operator ? (
        <form
          className="settings-form"
          onSubmit={(event) => {
            event.preventDefault();
            const untilMs = Date.now() + hours * 3_600_000;
            void runAction('silence:create', () => createSilence(agentId, qualifier.trim() && selectedKind?.qualifier ? `${indicator}:${qualifier.trim()}` : indicator, reason, untilMs), { inline: true }).then((saved) => {
              // Keep what the user typed when the save failed.
              if (saved) setReason('');
            });
          }}
        >
          <div className="form-grid">
            <label>
              Agent
              <select value={agentId} onChange={(e) => setAgentId(e.target.value)}>
                {global ? <option value="*">All agents</option> : null}
                {agents.map((a) => (
                  <option key={a.id} value={a.id}>{a.name || a.id}</option>
                ))}
              </select>
            </label>
            <label>
              Indicator
              <select value={indicator} onChange={(e) => { setIndicator(e.target.value); setQualifier(''); }}>
                <option value="*">All indicators</option>
                {kindList.map((kind) => (
                  <option key={kind.name} value={kind.name}>{kind.name}</option>
                ))}
              </select>
            </label>
            {selectedKind?.qualifier ? (
              <label>
                {`${selectedKind.qualifier.charAt(0).toUpperCase()}${selectedKind.qualifier.slice(1)} (optional, blank for all)`}
                <input value={qualifier} onChange={(e) => setQualifier(e.target.value)} />
              </label>
            ) : null}
            <label>
              Duration (hours)
              <input min={1} max={168} step={1} type="number" value={hours} onChange={(e) => setHours(Number(e.target.value))} />
            </label>
            <label>
              Reason
              <input placeholder="Planned maintenance…" value={reason} onChange={(e) => setReason(e.target.value)} />
            </label>
          </div>
          {kinds.error ? <LoadFailed what="indicator names" message={kinds.error} onRetry={() => void kinds.refresh()} stale={kinds.data !== null} /> : null}
          <ActionError actionKey="silence:create" />
          <div className="button-row">
            <button className="text-button" disabled={isBusy('silence:create')} type="submit">
              <Plus size={14} /> Add Silence Rule
            </button>
          </div>
        </form>
      ) : null}
      <div className="table-container">
        <table className="management-table">
          <thead>
            <tr>
              <th>Agent</th>
              <th>Indicator</th>
              <th>Reason</th>
              <th>Expires</th>
              <th>Created by</th>
              {operator ? <th>Delete</th> : null}
            </tr>
          </thead>
          <tbody>
            {silences.length === 0 && !loadFailed ? (
              <tr><td colSpan={operator ? 6 : 5} className="uptime">No active silence rules.</td></tr>
            ) : null}
            {silences.map((s) => {
              const agent = agents.find((a) => a.id === s.agent_id);
              const expired = s.until_ms <= Date.now();
              return (
                <tr key={s.silence_id} className={expired ? 'row-muted' : ''}>
                  <td>{s.agent_id === '*' ? 'All agents' : (agent?.name || s.agent_id)}</td>
                  <td>{s.indicator === '*' ? 'All indicators' : s.indicator}</td>
                  <td>{s.reason}</td>
                  <td>{new Date(s.until_ms).toLocaleString()}{expired ? ' (expired)' : ''}</td>
                  <td>{s.created_by}</td>
                  {operator ? (
                    <td>
                      <ActionButton
                        busy={isBusy(`silence:delete:${s.silence_id}`)}
                        danger
                        icon={<Trash2 size={14} />}
                        label="Delete"
                        onClick={() => runAction(`silence:delete:${s.silence_id}`, () => deleteSilence(s.silence_id))}
                      />
                    </td>
                  ) : null}
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </main>
  );
}

