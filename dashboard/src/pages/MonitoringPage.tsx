import React from 'react';
import { SlidersHorizontal } from 'lucide-react';
import { fetchLogMatches, fetchMetricHistory } from '../api';
import { AgentIdentity } from '../components/AgentIdentity';
import { Sparkline } from '../components/Sparkline';
import { StatusDot } from '../components/StatusDot';
import { StatusHistoryModal } from '../components/StatusHistoryModal';
import { usePolling } from '../hooks/usePolling';
import type { DashboardAgent, GroupRecord } from '../models';
import { groupOverviewAgents, hostStatusLabel, isUptimeAlarm, type OverviewGroupFilter } from '../status';
import { colorLabel } from '../uiLogic';

/** An expanded row refreshes its history on this interval, like the rest of the dashboard. */
const DETAIL_POLL_MS = 5000;

/**
 * Only the most recently expanded rows keep polling. Each polling row makes two requests per
 * interval, so three rows is at most about 1.2 requests a second per open tab however many rows
 * are expanded, which is enough to watch a few hosts side by side. The other rows keep their
 * last data and have a Refresh button. Polling also pauses while the tab is hidden.
 */
export const MAX_LIVE_DETAIL_ROWS = 3;

const COMPONENT_LABELS = ['CPU', 'Memory', 'Disk', 'Network', 'Proc', 'Heartbeat'];

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
  const [historyAgent, setHistoryAgent] = React.useState<{ id: string; name: string } | null>(null);

  const toggle = (id: string) =>
    setExpanded((current) => {
      const next = new Set(current);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });

  // `expanded` is in expansion order (a re-expanded row moves to the end), so the tail is the newest.
  const liveRows = new Set([...expanded].slice(-MAX_LIVE_DETAIL_ROWS));

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
                {group.agents.map((agent) => {
                  const open = expanded.has(agent.id);
                  return (
                    <React.Fragment key={`${group.id}:${agent.id}`}>
                      {/* The row is clickable for mouse users; the toggle button is the keyboard and screen reader control. */}
                      <tr className={open ? 'expanded' : ''} onClick={() => toggle(agent.id)}>
                        <td>
                          <AgentIdentity
                            id={agent.id}
                            name={agent.name}
                            prefix={
                              <button
                                aria-controls={`detail-${agent.id}`}
                                aria-expanded={open}
                                aria-label={`${open ? 'Collapse' : 'Expand'} ${agent.name}`}
                                className="row-toggle"
                                onClick={(event) => {
                                  event.stopPropagation(); // the row's own click would toggle a second time
                                  toggle(agent.id);
                                }}
                                type="button"
                              >
                                <span aria-hidden="true" className="chevron">&gt;</span>
                              </button>
                            }
                            secondary={agent.ipAddress || 'IP unknown'}
                          />
                        </td>
                        <td className={`uptime ${isUptimeAlarm(agent.uptimeSeconds) ? 'uptime-alarm' : ''}`}>{agent.uptime}</td>
                        <td className="dot-cell"><StatusDot color={agent.status} label={hostStatusLabel(agent)} /></td>
                        <td className="dot-cell"><StatusDot color={agent.alertColor} label={agent.alertColor === 'green' ? 'No alerts' : 'Alerts'} /></td>
                        {agent.components.map((component) => (
                          <td className="dot-cell" key={component.key}>
                            {/* Focusable and named in words, so the status does not depend on colour or hover. It must not toggle the row it sits in. */}
                            <button aria-label={`${component.label}: ${colorLabel(component.color)}, ${component.value}. ${component.detail}`} className="dot-wrap dot-button" onClick={(event) => event.stopPropagation()} onKeyDown={(event) => event.stopPropagation()} type="button">
                              <span aria-hidden="true" className={`dot ${component.color}`} />
                              <span aria-hidden="true" className="tooltip">
                                {component.label}: {colorLabel(component.color)}, {component.value}
                                <br />
                                <span>{component.detail}</span>
                              </span>
                            </button>
                          </td>
                        ))}
                      </tr>
                      {open ? (
                        <tr className="detail-row" id={`detail-${agent.id}`}>
                          <td colSpan={4 + COMPONENT_LABELS.length}>
                            <AgentDetail agent={agent} live={liveRows.has(agent.id)} onShowHistory={() => setHistoryAgent({ id: agent.id, name: agent.name })} />
                          </td>
                        </tr>
                      ) : null}
                    </React.Fragment>
                  );
                })}
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

/**
 * The expanded part of a row. It exists only while the row is open, and loads its metric history
 * and log matches on the poll interval, so the sparklines and log matches stay current. The two
 * load separately: one failing never hides the other, and each keeps its last good data.
 * A response that arrives after the row is closed is ignored (the request itself is not aborted),
 * and a slow response never overtakes a newer one. A row that is not live loads once and has a Refresh button.
 */
function AgentDetail({ agent, live, onShowHistory }: { agent: DashboardAgent; live: boolean; onShowHistory: () => void }) {
  const history = usePolling(React.useCallback((signal: AbortSignal) => fetchMetricHistory(agent.id, 20, signal), [agent.id]), { intervalMs: live ? DETAIL_POLL_MS : undefined });
  const logs = usePolling(React.useCallback((signal: AbortSignal) => fetchLogMatches(agent.id, 20, signal), [agent.id]), { intervalMs: live ? DETAIL_POLL_MS : undefined });
  const logMatches = logs.data ?? [];

  return (
    <div className="detail-inner">
      {history.error ? <div className="banner error detail-card-wide" role="alert">Could not load metric history for {agent.name}: {history.error}</div> : null}
      {logs.error ? <div className="banner error detail-card-wide" role="alert">Could not load log matches for {agent.name}: {logs.error}</div> : null}
      {agent.components.map((component) => {
        const sparkData = history.data
          ? component.key === 'cpu'
            ? history.data.map((snapshot) => snapshot.metrics.cpu.usage_percent).reverse()
            : component.key === 'memory'
              ? history.data.map((snapshot) => snapshot.metrics.memory.usage_percent).reverse()
              : []
          : [];
        return (
          <div className="detail-card" key={component.key}>
            <div className="detail-card-header">
              <span>{component.label}</span>
              <span aria-hidden="true" className={`dot ${component.color} small-dot`} />
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
        <button className="text-button" onClick={onShowHistory}>View status history</button>
        {live ? null : (
          <>
            <div className="detail-card-sub">Not updating automatically: only the {MAX_LIVE_DETAIL_ROWS} most recently opened rows do.</div>
            <button className="text-button" onClick={() => void Promise.all([history.refresh(), logs.refresh()])}>Refresh</button>
          </>
        )}
      </div>
      {logMatches.length > 0 ? (
        <div className="detail-card detail-card-wide">
          <div className="detail-card-header"><span>Log Matches</span></div>
          <table className="history-table">
            <tbody>
              {logMatches.slice(0, 10).map((row) => (
                <tr key={row.match_id}>
                  <td className="history-ts">{new Date(row.created_at).toLocaleString()}</td>
                  <td>{row.indicator_name}</td>
                  <td><span aria-hidden="true" className={`dot ${row.severity} small-dot`} /> <span className="sr-only">{row.severity}</span></td>
                  <td className="history-msg" title={row.matched_line}>{row.path}: {row.matched_line.slice(0, 80)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ) : null}
    </div>
  );
}
