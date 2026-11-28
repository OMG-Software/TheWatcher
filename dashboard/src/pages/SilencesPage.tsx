import React from 'react';
import { Plus, Trash2 } from 'lucide-react';
import { createSilence, deleteSilence } from '../api';
import { ActionError } from '../components/ActionError';
import { ActionButton } from '../components/ActionButton';
import { useNow } from '../hooks/useNow';
import type { RunAction } from '../hooks/useRunAction';
import type { DashboardAgent, SilenceRecord } from '../models';

export function SilencesPage({
  agents,
  isBusy,
  global,
  loadFailed = false,
  operator,
  runAction,
  silences,
}: {
  agents: DashboardAgent[];
  isBusy: (key: string) => boolean;
  global: boolean;
  loadFailed?: boolean;
  operator: boolean;
  runAction: RunAction;
  silences: SilenceRecord[];
}) {
  const [agentChoice, setAgentId] = React.useState('*');
  // A group user cannot target "all agents", so fall back to their first agent.
  const agentId = !global && agentChoice === '*' && agents.length > 0 ? agents[0].id : agentChoice;
  const [indicator, setIndicator] = React.useState('*');
  const [reason, setReason] = React.useState('');
  const [hours, setHours] = React.useState(1);
  const now = useNow();

  return (
    <main className="table-wrap management-wrap">
      <div className="management-header"><h1>Silence Rules</h1></div>
      {operator ? (
        <form
          className="settings-form"
          onSubmit={(event) => {
            event.preventDefault();
            const untilMs = Date.now() + hours * 3_600_000;
            void runAction('silence:create', () => createSilence(agentId, indicator, reason, untilMs), { inline: true }).then((saved) => {
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
              <select value={indicator} onChange={(e) => setIndicator(e.target.value)}>
                <option value="*">All indicators</option>
                <option value="cpu">cpu</option>
                <option value="memory">memory</option>
                <option value="disk">disk</option>
                <option value="network">network</option>
                <option value="processes">processes</option>
                <option value="heartbeat">heartbeat</option>
              </select>
            </label>
            <label>
              Duration (hours)
              <input min={1} max={168} step={1} type="number" value={hours} onChange={(e) => setHours(Number(e.target.value))} />
            </label>
            <label>
              Reason
              <input placeholder="Planned maintenance…" value={reason} onChange={(e) => setReason(e.target.value)} />
            </label>
          </div>
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
              const expired = s.until_ms <= now;
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

