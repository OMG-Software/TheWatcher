import { Check, X } from 'lucide-react';
import { approveAgent, rejectAgent } from '../api';
import { ActionButton } from './ActionButton';
import { AgentIdentity } from './AgentIdentity';
import type { RunAction } from '../hooks/useRunAction';
import type { DashboardAgent, GroupRecord } from '../models';

export function PendingEnrollments({ agents, busyAction, groups, runAction }: { agents: DashboardAgent[]; busyAction: string | null; groups: GroupRecord[]; runAction: RunAction }) {
  const defaultGroup = groups[0]?.group_id;
  return (
    <section className="embedded-management">
      <div className="management-header"><h1>Pending Enrollments</h1></div>
      <div className="table-container">
        <table className="management-table">
          <thead><tr><th>Agent</th><th>Platform</th><th>Last Seen</th><th>Actions</th></tr></thead>
          <tbody>{agents.map((agent) => (
            <tr key={agent.id}>
              <td><AgentIdentity id={agent.id} name={agent.name} secondary={`${agent.ipAddress || 'IP unknown'} / ${agent.id}`} /></td>
              <td>{agent.platform}</td>
              <td className="uptime">{agent.lastSeen > 0 ? `${Math.floor((Date.now() - agent.lastSeen) / 60000)}m` : 'never'}</td>
              <td><div className="button-row">
                <ActionButton busy={busyAction === `${agent.id}:approve`} icon={<Check size={14} />} label="Approve" onClick={() => runAction(`${agent.id}:approve`, () => approveAgent(agent.id, defaultGroup ? [defaultGroup] : []))} />
                <ActionButton busy={busyAction === `${agent.id}:reject`} icon={<X size={14} />} label="Reject" onClick={() => runAction(`${agent.id}:reject`, () => rejectAgent(agent.id))} />
              </div></td>
            </tr>
          ))}
          {agents.length === 0 ? <tr><td colSpan={4} className="uptime">No pending enrollments.</td></tr> : null}
          </tbody>
        </table>
      </div>
    </section>
  );
}

