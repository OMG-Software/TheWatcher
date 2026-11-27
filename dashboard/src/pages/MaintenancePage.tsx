import React from 'react';
import { Plus, Trash2 } from 'lucide-react';
import { createMaintenanceWindow, deleteMaintenanceWindow } from '../api';
import { ActionError } from '../components/ActionError';
import { ActionButton } from '../components/ActionButton';
import { AgentIdentity } from '../components/AgentIdentity';
import type { RunAction } from '../hooks/useRunAction';
import type { DashboardAgent, MaintenanceWindowRecord } from '../models';

export function MaintenancePage({
  agents,
  busyAction,
  global,
  loadFailed = false,
  operator,
  runAction,
  windows,
}: {
  agents: DashboardAgent[];
  busyAction: string | null;
  global: boolean;
  loadFailed?: boolean;
  operator: boolean;
  runAction: RunAction;
  windows: MaintenanceWindowRecord[];
}) {
  const [agentId, setAgentId] = React.useState('*');
  const [reason, setReason] = React.useState('');
  const [startDate, setStartDate] = React.useState('');
  const [startTime, setStartTime] = React.useState('00:00');
  const [durationHours, setDurationHours] = React.useState(1);

  const fmt = (ms: number) => new Date(ms).toLocaleString();

  React.useEffect(() => {
    if (!global && agentId === '*' && agents.length > 0) setAgentId(agents[0].id);
  }, [agentId, agents, global]);

  return (
    <main className="table-wrap management-wrap">
      <div className="management-header"><h1>Maintenance Windows</h1></div>
      {operator ? (
        <form
          className="inline-form maint-form"
          onSubmit={(event) => {
            event.preventDefault();
            if (!startDate) return;
            const startMs = new Date(`${startDate}T${startTime}`).getTime();
            const endMs = startMs + durationHours * 3600_000;
            if (Number.isNaN(startMs) || endMs <= startMs) return;
            void runAction('mw:create', () => createMaintenanceWindow(agentId, startMs, endMs, reason), { inline: true }).then((saved) => {
              // Keep what the user typed when the save failed.
              if (saved) setReason('');
            });
          }}
        >
          <select aria-label="Agent" value={agentId} onChange={(event) => setAgentId(event.target.value)}>
            {global ? <option value="*">All agents</option> : null}
            {agents.map((a) => <option key={a.id} value={a.id}>{a.name || a.id}</option>)}
          </select>
          <input
            aria-label="Reason"
            placeholder="Reason"
            value={reason}
            onChange={(event) => setReason(event.target.value)}
          />
          <input
            aria-label="Start date"
            type="date"
            value={startDate}
            onChange={(event) => setStartDate(event.target.value)}
          />
          <input
            aria-label="Start time"
            type="time"
            value={startTime}
            onChange={(event) => setStartTime(event.target.value)}
          />
          <label>
            Duration (h)
            <input
              aria-label="Duration hours"
              min={1}
              step={1}
              type="number"
              value={durationHours}
              onChange={(event) => setDurationHours(Math.max(1, Number(event.target.value)))}
              className="duration-input"
            />
          </label>
          <ActionButton busy={busyAction === 'mw:create'} disabled={!operator} icon={<Plus size={14} />} label="Schedule" onClick={() => undefined} type="submit" />
        </form>
      ) : null}
      <ActionError actionKey="mw:create" />
      <div className="table-container">
        <table className="management-table">
          <thead>
            <tr>
              <th>Agent</th>
              <th>Start</th>
              <th>End</th>
              <th>Reason</th>
              <th>Created by</th>
              <th>Delete</th>
            </tr>
          </thead>
          <tbody>
            {windows.length === 0 && !loadFailed ? (
              <tr><td colSpan={6} className="uptime">No maintenance windows scheduled.</td></tr>
            ) : null}
            {windows.map((w) => {
              const agent = agents.find((a) => a.id === w.agent_id);
              const now = Date.now();
              const active = w.start_ms <= now && w.end_ms > now;
              return (
                <tr key={w.window_id} className={active ? 'maint-active' : ''}>
                  <td>{w.agent_id === '*' ? <em>All agents</em> : <AgentIdentity id={w.agent_id} name={agent?.name ?? w.agent_id} />}</td>
                  <td className="uptime">{fmt(w.start_ms)}</td>
                  <td className="uptime">{fmt(w.end_ms)}</td>
                  <td>{w.reason}</td>
                  <td>{w.created_by}</td>
                  <td>
                    <ActionButton
                      busy={busyAction === `mw:delete:${w.window_id}`}
                      danger
                      disabled={!operator}
                      icon={<Trash2 size={14} />}
                      label="Delete"
                      onClick={() => runAction(`mw:delete:${w.window_id}`, () => deleteMaintenanceWindow(w.window_id))}
                    />
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </main>
  );
}

