import React from 'react';
import { Check, X } from 'lucide-react';
import { approveAgent, rejectAgent } from '../api';
import { ActionButton } from './ActionButton';
import { AgentIdentity } from './AgentIdentity';
import type { RunAction } from '../hooks/useRunAction';
import type { DashboardAgent, GroupRecord } from '../models';

export function PendingEnrollments({ agents, busyAction, groups, loadFailed = false, runAction }: { agents: DashboardAgent[]; busyAction: string | null; groups: GroupRecord[]; loadFailed?: boolean; runAction: RunAction }) {
  // The group each pending agent will be approved into. Nothing is chosen by
  // default: an agent with no group is visible to global users only, so an
  // operator has to name a group on purpose. Group ids start at 1, but compare
  // with null/undefined rather than truthiness so no id is ever skipped.
  const [chosenGroup, setChosenGroup] = React.useState<Record<string, number | null>>({});
  return (
    <section className="embedded-management">
      <div className="management-header"><h1>Pending Enrollments</h1></div>
      <div className="table-container">
        <table className="management-table">
          <thead><tr><th>Agent</th><th>Platform</th><th>Last Seen</th><th>Group</th><th>Actions</th></tr></thead>
          <tbody>{agents.map((agent) => (
            <tr key={agent.id}>
              <td><AgentIdentity id={agent.id} name={agent.name} secondary={`${agent.ipAddress || 'IP unknown'} / ${agent.id}`} /></td>
              <td>{agent.platform}</td>
              <td className="uptime">{agent.lastSeen > 0 ? `${Math.floor((Date.now() - agent.lastSeen) / 60000)}m` : 'never'}</td>
              <td>
                <select
                  aria-label={`Group for ${agent.name || agent.id}`}
                  value={chosenGroup[agent.id] == null ? '' : String(chosenGroup[agent.id])}
                  onChange={(event) => setChosenGroup((current) => ({ ...current, [agent.id]: event.target.value === '' ? null : Number(event.target.value) }))}
                >
                  <option value="">No group (global users only)</option>
                  {groups.map((group) => <option key={group.group_id} value={group.group_id}>{group.name}</option>)}
                </select>
              </td>
              <td><div className="button-row">
                <ActionButton busy={busyAction === `${agent.id}:approve`} icon={<Check size={14} />} label="Approve" onClick={() => runAction(`${agent.id}:approve`, () => approveAgent(agent.id, chosenGroup[agent.id] == null ? [] : [chosenGroup[agent.id] as number]))} />
                <ActionButton busy={busyAction === `${agent.id}:reject`} icon={<X size={14} />} label="Reject" onClick={() => runAction(`${agent.id}:reject`, () => rejectAgent(agent.id))} />
              </div></td>
            </tr>
          ))}
          {agents.length === 0 && !loadFailed ? <tr><td colSpan={5} className="uptime">No pending enrollments.</td></tr> : null}
          </tbody>
        </table>
      </div>
    </section>
  );
}

