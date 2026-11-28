import React from 'react';
import { SlidersHorizontal } from 'lucide-react';
import { errorMessage, fetchLogMatches, fetchMetricHistory } from '../api';
import { AgentIdentity } from '../components/AgentIdentity';
import { Sparkline } from '../components/Sparkline';
import { StatusDot } from '../components/StatusDot';
import { StatusHistoryModal } from '../components/StatusHistoryModal';
import type { DashboardAgent, GroupRecord, LogMatchRecord, MetricsSnapshot } from '../models';
import { groupOverviewAgents, isUptimeAlarm, type OverviewGroupFilter } from '../status';
import { colorLabel } from '../uiLogic';

export function MonitoringPage({
  agents,
  expanded,
  global,
  groupFilter,
  groups,
  setExpanded,
  setGroupFilter,
}: {
  agents: DashboardAgent[];
  expanded: Set<string>;
  global: boolean;
  groupFilter: OverviewGroupFilter;
  groups: GroupRecord[];
  setExpanded: React.Dispatch<React.SetStateAction<Set<string>>>;
  setGroupFilter: React.Dispatch<React.SetStateAction<OverviewGroupFilter>>;
}) {
  const [agentHistory, setAgentHistory] = React.useState<Record<string, MetricsSnapshot[]>>({});
  const [agentLogMatches, setAgentLogMatches] = React.useState<Record<string, LogMatchRecord[]>>({});
  // History and log matches load separately, so one failing never hides the other.
  const [historyErrors, setHistoryErrors] = React.useState<Record<string, string>>({});
  const [logErrors, setLogErrors] = React.useState<Record<string, string>>({});
  const [historyAgent, setHistoryAgent] = React.useState<{ id: string; name: string } | null>(null);
  const requested = React.useRef(new Set<string>()); // agents whose detail is loaded or loading

  // Load detail once per expanded agent. Each part keeps its own result and error; an error is
  // shown in the detail row, and the agent is loaded again the next time its row is expanded.
  React.useEffect(() => {
    const track = <T,>(id: string, load: Promise<T>, onData: (value: T) => void, setErrors: React.Dispatch<React.SetStateAction<Record<string, string>>>, fallback: string) =>
      load.then(
        (value) => {
          onData(value);
          setErrors((prev) => withoutKey(prev, id));
          return true;
        },
        (err: unknown) => {
          setErrors((prev) => ({ ...prev, [id]: errorMessage(err, fallback) }));
          return false;
        },
      );
    for (const id of expanded) {
      if (requested.current.has(id)) continue;
      requested.current.add(id);
      void Promise.all([
        track(id, fetchMetricHistory(id, 20), (snapshots) => setAgentHistory((prev) => ({ ...prev, [id]: snapshots })), setHistoryErrors, 'Failed to load history'),
        track(id, fetchLogMatches(id, 20), (rows) => setAgentLogMatches((prev) => ({ ...prev, [id]: rows })), setLogErrors, 'Failed to load log matches'),
      ]).then((results) => {
        if (!results.every(Boolean)) requested.current.delete(id);
      });
    }
  }, [expanded]);

  const overviewGroups = groupOverviewAgents(agents, groups, groupFilter);
  const visibleAgentCount = overviewGroups.reduce((count, group) => count + group.agents.length, 0);

  return (
    <main className="table-wrap">
      {agents.length === 0 ? <div className="banner">No approved agents are reporting yet.</div> : null}
      {agents.length > 0 && visibleAgentCount === 0 ? <div className="banner">No agents match this group filter.</div> : null}

      {global ? (
        <div className="overview-toolbar">
          <label>
            <SlidersHorizontal size={14} />
            <span>Group</span>
            <select value={groupFilter} onChange={(event) => setGroupFilter(event.target.value)}>
              <option value="all">All groups</option>
              {groups.map((group) => (
                <option key={group.group_id} value={String(group.group_id)}>{group.name}</option>
              ))}
              <option value="ungrouped">Ungrouped</option>
            </select>
          </label>
        </div>
      ) : null}

      <div className="table-container">
        <table>
          <thead>
            <tr>
              <th>Server</th>
              <th>Uptime</th>
              <th>Status</th>
              <th>Alerts</th>
              {COMPONENT_LABELS.map((label) => (
                <th key={label}>{label}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {overviewGroups.map((group) => (
              <React.Fragment key={group.id}>
                <tr className="section-header">
                  <td colSpan={4 + COMPONENT_LABELS.length}>
                    {group.name} <span>{group.agents.length}</span>
                  </td>
                </tr>
                {group.agents.map((agent) => (
                  <React.Fragment key={`${group.id}:${agent.id}`}>
                    <tr
                      className={expanded.has(agent.id) ? 'expanded' : ''}
                      onClick={() => {
                        setExpanded((current) => {
                          const next = new Set(current);
                          if (next.has(agent.id)) next.delete(agent.id);
                          else next.add(agent.id);
                          return next;
                        });
                      }}
                    >
                      <td><AgentIdentity id={agent.id} name={agent.name} prefix={<span className="chevron">&gt;</span>} secondary={agent.ipAddress || 'IP unknown'} /></td>
                      <td className={`uptime ${isUptimeAlarm(agent.uptimeSeconds) ? 'uptime-alarm' : ''}`}>{agent.uptime}</td>
                      <td className="dot-cell"><StatusDot color={agent.status} label={colorLabel(agent.status)} /></td>
                      <td className="dot-cell"><StatusDot color={agent.alertColor} label="Alerts" /></td>
                      {agent.components.map((component) => (
                        <td className="dot-cell" key={component.key}>
                          <div className="dot-wrap">
                            <span className={`dot ${component.color}`} />
                            <div className="tooltip">
                              {component.label}: {component.value}
                              <br />
                              <span>{component.detail}</span>
                            </div>
                          </div>
                        </td>
                      ))}
                    </tr>
                    <tr className={`detail-row ${expanded.has(agent.id) ? '' : 'hidden'}`}>
                      <td colSpan={4 + COMPONENT_LABELS.length}>
                        <div className="detail-inner">
                          {historyErrors[agent.id] ? <div className="banner error detail-card-wide" role="alert">Could not load metric history for {agent.name}: {historyErrors[agent.id]}</div> : null}
                          {logErrors[agent.id] ? <div className="banner error detail-card-wide" role="alert">Could not load log matches for {agent.name}: {logErrors[agent.id]}</div> : null}
                          {agent.components.map((component) => {
                            const history = agentHistory[agent.id];
                            const sparkData = history
                              ? component.key === 'cpu'
                                ? history.map((s) => s.metrics.cpu.usage_percent).reverse()
                                : component.key === 'memory'
                                  ? history.map((s) => s.metrics.memory.usage_percent).reverse()
                                  : []
                              : [];
                            return (
                              <div className="detail-card" key={component.key}>
                                <div className="detail-card-header">
                                  <span>{component.label}</span>
                                  <span className={`dot ${component.color} small-dot`} />
                                </div>
                                <div className={`detail-card-value ${component.color}`}>{component.value}</div>
                                <div className="detail-card-sub">
                                  {colorLabel(component.color)} / {component.detail}
                                </div>
                                {sparkData.length >= 2 ? (
                                  <div className="sparkline-wrap">
                                    <Sparkline data={sparkData} color={`var(--${component.color === 'green' ? 'green' : component.color === 'yellow' ? 'yellow' : component.color === 'amber' ? 'amber' : component.color === 'red' ? 'red' : 'muted'})`} />
                                  </div>
                                ) : null}
                              </div>
                            );
                          })}
                          <div className="detail-card">
                            <div className="detail-card-header"><span>Uptime</span></div>
                            <div className={`detail-card-value ${isUptimeAlarm(agent.uptimeSeconds) ? 'red' : 'green'}`}>
                              {agent.uptime}
                            </div>
                            <div className="detail-card-sub">Since last restart</div>
                          </div>
                          <div className="detail-card">
                            <div className="detail-card-header"><span>Status History</span></div>
                            <button
                              className="text-button"
                              onClick={(event) => {
                                event.stopPropagation();
                                setHistoryAgent({ id: agent.id, name: agent.name });
                              }}
                            >
                              View status history
                            </button>
                          </div>
                          {(agentLogMatches[agent.id]?.length ?? 0) > 0 ? (
                            <div className="detail-card detail-card-wide">
                              <div className="detail-card-header"><span>Log Matches</span></div>
                              <table className="history-table">
                                <tbody>
                                  {agentLogMatches[agent.id].slice(0, 10).map((row) => (
                                    <tr key={row.match_id}>
                                      <td className="history-ts">{new Date(row.created_at).toLocaleString()}</td>
                                      <td>{row.indicator_name}</td>
                                      <td><span className={`dot ${row.severity} small-dot`} /></td>
                                      <td className="history-msg" title={row.matched_line}>{row.path}: {row.matched_line.slice(0, 80)}</td>
                                    </tr>
                                  ))}
                                </tbody>
                              </table>
                            </div>
                          ) : null}
                        </div>
                      </td>
                    </tr>
                  </React.Fragment>
                ))}
              </React.Fragment>
            ))}
          </tbody>
        </table>
      </div>
      {historyAgent ? (
        <StatusHistoryModal
          agentId={historyAgent.id}
          agentName={historyAgent.name}
          onClose={() => setHistoryAgent(null)}
        />
      ) : null}
    </main>
  );
}

function withoutKey(record: Record<string, string>, key: string): Record<string, string> {
  const rest = { ...record };
  delete rest[key];
  return rest;
}

const COMPONENT_LABELS = ['CPU', 'Memory', 'Disk', 'Network', 'Proc', 'Heartbeat'];

