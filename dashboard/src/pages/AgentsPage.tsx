import React from 'react';
import { Check, Pause, Play, RotateCw, Search, SlidersHorizontal, Trash2 } from 'lucide-react';
import { canManageAgent, maintenanceAction, maintenanceActionLabel } from '../agentActions';
import { deleteAgent, requestAgentStatus, restartAgentCollectors, resumeAgent, setAgentDescription, setMaintenance } from '../api';
import { ActionButton } from '../components/ActionButton';
import { AgentIdentity } from '../components/AgentIdentity';
import { PendingEnrollments } from '../components/PendingEnrollments';
import { StatusDot } from '../components/StatusDot';
import type { RunAction } from '../hooks/useRunAction';
import type { DashboardAgent, GroupRecord } from '../models';
import { colorLabel } from '../uiLogic';

export function AgentsPage({
  agents,
  busyAction,
  groups,
  onConfigure,
  operator,
  pending,
  pendingLoadFailed,
  showPending,
  runAction,
}: {
  agents: DashboardAgent[];
  busyAction: string | null;
  groups: GroupRecord[];
  onConfigure: (agentId: string) => void;
  operator: boolean;
  pending: DashboardAgent[];
  pendingLoadFailed: boolean;
  showPending: boolean;
  runAction: RunAction;
}) {
  const [descDrafts, setDescDrafts] = React.useState<Record<string, string>>({});

  return (
    <main className="table-wrap management-wrap">
      {showPending ? <PendingEnrollments agents={pending} busyAction={busyAction} groups={groups} loadFailed={pendingLoadFailed} runAction={runAction} /> : null}
      <div className="management-header"><h1>Agent Management</h1></div>
      <div className="table-container">
        <table className="management-table">
          <thead>
            <tr>
              <th>Agent</th>
              <th>Platform</th>
              <th>State</th>
              <th>Last Seen</th>
              <th>Groups</th>
              <th>Commands</th>
              <th>Configure</th>
              <th>Delete</th>
            </tr>
          </thead>
          <tbody>
            {agents.map((agent) => {
              const descValue = descDrafts[agent.id] ?? agent.description;
              const manageable = operator && canManageAgent(agent);
              const maintenanceCommand = maintenanceAction(agent);
              return (
                <tr key={agent.id}>
                  <td>
                    <div className="agent-config-cell">
                      <AgentIdentity id={agent.id} name={agent.name} secondary={`${agent.ipAddress || 'IP unknown'} / ${agent.id}`} />
                      <div className="settings-grid desc-settings">
                        <input
                          disabled={!operator}
                          placeholder="Description…"
                          value={descValue}
                          onChange={(event) => setDescDrafts((current) => ({ ...current, [agent.id]: event.target.value }))}
                        />
                        <ActionButton
                          busy={busyAction === `${agent.id}:desc`}
                          disabled={!operator || descValue === agent.description}
                          icon={<Check size={14} />}
                          label="Set"
                          onClick={() => runAction(`${agent.id}:desc`, () => setAgentDescription(agent.id, descValue))}
                        />
                      </div>
                    </div>
                  </td>
                  <td>{agent.platform}</td>
                  <td><StatusDot color={agent.status} label={colorLabel(agent.status)} /></td>
                  <td className="uptime">{agent.lastSeen > 0 ? `${Math.floor((Date.now() - agent.lastSeen) / 60000)}m` : 'never'}</td>
                  <td>{agent.groupIds.map((id) => groups.find((group) => group.group_id === id)?.name ?? id).join(', ') || 'No group'}</td>
                  <td>
                    <div className="button-row">
                      <ActionButton busy={busyAction === `${agent.id}:${maintenanceCommand}`} disabled={!manageable} icon={agent.maintenance ? <Play size={14} /> : <Pause size={14} />} label={maintenanceActionLabel(agent)} onClick={() => runAction(`${agent.id}:${maintenanceCommand}`, () => agent.maintenance ? resumeAgent(agent.id) : setMaintenance(agent.id, 'operator request', 0))} />
                      <ActionButton busy={busyAction === `${agent.id}:restart`} disabled={!manageable} icon={<RotateCw size={14} />} label="Restart" onClick={() => runAction(`${agent.id}:restart`, () => restartAgentCollectors(agent.id))} />
                      <ActionButton busy={busyAction === `${agent.id}:status`} disabled={!manageable} icon={<Search size={14} />} label="Status" onClick={() => runAction(`${agent.id}:status`, () => requestAgentStatus(agent.id))} />
                    </div>
                  </td>
                  <td>
                    <ActionButton
                      busy={busyAction === `${agent.id}:configure`}
                      disabled={!manageable}
                      icon={<SlidersHorizontal size={14} />}
                      label="Configure"
                      onClick={() => onConfigure(agent.id)}
                    />
                  </td>
                  <td><ActionButton busy={busyAction === `${agent.id}:delete`} danger disabled={!manageable} icon={<Trash2 size={14} />} label="Delete" onClick={() => runAction(`${agent.id}:delete`, () => deleteAgent(agent.id))} /></td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </main>
  );
}

