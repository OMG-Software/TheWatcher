import React from 'react';
import { createRoot } from 'react-dom/client';
import { Archive, BookOpen, Check, CheckSquare, LogOut, Pause, Play, Plus, RefreshCw, RotateCw, Search, SlidersHorizontal, Square, Trash2, X } from 'lucide-react';
import { canManageAgent, maintenanceAction, maintenanceActionLabel } from './agentActions';
import {
  acknowledgeAlert,
  archiveAlert,
  approveAgent,
  bulkAcknowledgeAlerts,
  bulkArchiveAlerts,
  changeUserPassword,
  createGroup,
  createMaintenanceWindow,
  createSilence,
  createUser,
  createRunbook,
  createView,
  deleteAgent,
  deleteMaintenanceWindow,
  deleteRunbook,
  deleteSilence,
  deleteUser,
  deleteView,
  fetchRunbooks,
  disableUser,
  enableUser,
  fetchAgentHistory,
  fetchAlerts,
  fetchLogMatches,
  fetchMetricHistory,
  fetchSession,
  fetchSettings,
  fetchViews,
  loadDashboardData,
  login,
  logout,
  rejectAgent,
  requestAgentStatus,
  restartAgentCollectors,
  resumeAgent,
  setAgentCollectorConfig,
  setAgentDescription,
  sendReport,
  setMaintenance,
  updateSettings,
  updateView,
} from './api';
import type {
  AgentCollectorConfigUpdate,
  AlertRecord,
  AnomalyConfig,
  CollectorConfig,
  DashboardAgent,
  DiskMonitorConfig,
  GroupRecord,
  HealthColor,
  LogMatchRecord,
  LogMonitorConfig,
  MaintenanceWindowRecord,
  MetricsSnapshot,
  NetworkInterfaceConfig,
  ProcessWatchConfig,
  RunbookRecord,
  ServerSettings,
  SessionInfo,
  SilenceRecord,
  StatusHistoryRow,
  UserRecord,
  ViewRecord,
} from './models';
import {
  DEFAULT_ANOMALY_CONFIG,
  DEFAULT_NETWORK_THRESHOLDS,
  DEFAULT_PERCENT_THRESHOLDS,
  collectorConfigWithDefaults,
  groupOverviewAgents,
  isUptimeAlarm,
  summaryCounts,
  toDashboardAgents,
  toDisplayAlerts,
  type OverviewGroupFilter,
} from './status';
import { NAV_ITEMS, accountTypeLabel, parseMarkdownBlocks, safeMarkdownHref, type DashboardView } from './uiLogic';
import './styles.css';

const COMPONENT_LABELS = ['CPU', 'Memory', 'Disk', 'Network', 'Proc', 'Heartbeat'];

type View = DashboardView;
type RunAction = (key: string, action: () => Promise<void>) => Promise<boolean>;

function isGlobal(role: SessionInfo['role']): boolean {
  return role.startsWith('global_');
}

function canOperate(role: SessionInfo['role']): boolean {
  return role === 'global_admin' || role === 'global_operator' || role === 'group_admin' || role === 'group_operator';
}

function canManageUsers(role: SessionInfo['role']): boolean {
  return role === 'global_admin' || role === 'group_admin';
}

function canManageGroups(role: SessionInfo['role']): boolean {
  return role === 'global_admin';
}

function colorLabel(color: HealthColor): string {
  return {
    green: 'Healthy',
    yellow: 'Warning',
    amber: 'Degraded',
    red: 'Critical',
    grey: 'No Data',
    blue: 'Maintenance',
  }[color];
}

function Dashboard() {
  const [session, setSession] = React.useState<SessionInfo | null>(null);
  const [view, setView] = React.useState<View>('monitoring');
  const [agents, setAgents] = React.useState<DashboardAgent[]>([]);
  const [pending, setPending] = React.useState<DashboardAgent[]>([]);
  const [groups, setGroups] = React.useState<GroupRecord[]>([]);
  const [alerts, setAlerts] = React.useState<AlertRecord[]>([]);
  const [allAlerts, setAllAlerts] = React.useState<AlertRecord[]>([]);
  const [users, setUsers] = React.useState<UserRecord[]>([]);
  const [maintenanceWindows, setMaintenanceWindows] = React.useState<MaintenanceWindowRecord[]>([]);
  const [silences, setSilences] = React.useState<SilenceRecord[]>([]);
  const [customViews, setCustomViews] = React.useState<ViewRecord[]>([]);
  const [activeViewId, setActiveViewId] = React.useState<number | null>(() => {
    const hash = window.location.hash;
    const m = hash.match(/^#view-(\d+)$/);
    return m ? parseInt(m[1], 10) : null;
  });
  const [expanded, setExpanded] = React.useState<Set<string>>(new Set());
  const [loadedAt, setLoadedAt] = React.useState<Date | null>(null);
  const [error, setError] = React.useState<string | null>(null);
  const [loading, setLoading] = React.useState(true);
  const [busyAction, setBusyAction] = React.useState<string | null>(null);
  const [configAgentId, setConfigAgentId] = React.useState<string | null>(null);
  const [overviewGroupFilter, setOverviewGroupFilter] = React.useState<OverviewGroupFilter>('all');

  const refresh = React.useCallback(async () => {
    if (!session) return;
    try {
      setError(null);
      const [data, views] = await Promise.all([loadDashboardData(), fetchViews().catch(() => [] as ViewRecord[])]);
      setAgents(toDashboardAgents(data.agents, data.metrics, data.alerts));
      setPending(toDashboardAgents(data.pending, [], []));
      setGroups(data.groups);
      setAlerts(data.alerts);
      setAllAlerts(data.allAlerts);
      setUsers(data.users);
      setMaintenanceWindows(data.maintenanceWindows);
      setSilences(data.silences);
      setCustomViews(views);
      setLoadedAt(new Date());
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to load dashboard data');
    } finally {
      setLoading(false);
    }
  }, [session]);

  React.useEffect(() => {
    void fetchSession()
      .then(setSession)
      .catch(() => setSession(null))
      .finally(() => setLoading(false));
  }, []);

  React.useEffect(() => {
    void refresh();
    const timer = window.setInterval(() => void refresh(), 5000);
    return () => window.clearInterval(timer);
  }, [refresh]);

  // Sync activeViewId to URL hash so views are bookmarkable.
  React.useEffect(() => {
    if (activeViewId != null) {
      window.location.hash = `view-${activeViewId}`;
    } else if (window.location.hash.startsWith('#view-')) {
      window.history.replaceState(null, '', window.location.pathname + window.location.search);
    }
  }, [activeViewId]);

  const runAction = React.useCallback(
    async (key: string, action: () => Promise<void>) => {
      try {
        setBusyAction(key);
        setError(null);
        await action();
        await refresh();
        return true;
      } catch (err) {
        setError(err instanceof Error ? err.message : 'Action failed');
        return false;
      } finally {
        setBusyAction(null);
      }
    },
    [refresh],
  );

  if (!session) {
    return <LoginScreen onLogin={setSession} />;
  }

  const counts = summaryCounts(agents);
  const displayAlerts = toDisplayAlerts(alerts, agents);
  const global = isGlobal(session.role);
  const admin = canManageUsers(session.role);
  const operator = canOperate(session.role);
  const configAgent = agents.find((agent) => agent.id === configAgentId) ?? null;

  return (
    <>
      <header className="topbar">
        <div className="logo">
          <span className="logo-dot" />
          THEWATCHER
        </div>
        <nav className="nav-tabs" aria-label="Dashboard views">
          {NAV_ITEMS.map((item) => {
            if (item.requiresOperator && !operator) return null;
            if (item.requiresGlobalOperator && !(global && operator)) return null;
            const label = item.view === 'views' ? `Views${customViews.length > 0 ? ` (${customViews.length})` : ''}` : item.label;
            return <Tab key={item.view} view={view} target={item.view} setView={setView} label={label} />;
          })}
        </nav>
        <div className="topbar-meta">
          <span>
            <strong>{loadedAt ? loadedAt.toUTCString().replace('GMT', 'UTC') : 'Not synced'}</strong>
          </span>
          <span>
            {session.username} <strong>{accountTypeLabel(session.role)}</strong>
          </span>
          <button className="icon-button" onClick={() => void refresh()} aria-label="Refresh dashboard">
            <RefreshCw size={14} />
          </button>
          <button
            className="icon-button"
            onClick={() => {
              // Clear session locally regardless of server response so the UI
              // returns to the login screen immediately. Without this, an error
              // (or even a missing .catch on the previous version) would leave
              // the user on the dashboard until they refreshed the page.
              void logout().catch(() => undefined);
              setSession(null);
            }}
            aria-label="Log out"
          >
            <LogOut size={14} />
          </button>
        </div>
      </header>

      <section className="summary">
        <SummaryCard label="Healthy" value={counts.green} color="green" />
        <SummaryCard label="Warning" value={counts.yellow} color="yellow" />
        <SummaryCard label="Degraded" value={counts.amber} color="amber" />
        <SummaryCard label="Critical" value={counts.red} color="red" />
        <SummaryCard label="Maintenance" value={counts.blue} color="blue" />
        <SummaryCard label="Offline" value={counts.offline} color="grey" />
      </section>

      {alerts.length > 0 && view === 'monitoring' ? (
        <div className="alert-strip">
          {displayAlerts.slice(0, 4).map((alert) => (
            <div className={`alert-card ${alert.new_status}`} key={alert.alert_id}>
              <AgentIdentity id={alert.agentId} name={alert.agentName} />
              <div className="alert-card-detail">
                <span>{alert.indicator}</span>
                <strong>{colorLabel(alert.new_status)}</strong>
              </div>
              <div className="alert-card-message">{alert.message}</div>
            </div>
          ))}
        </div>
      ) : null}

      {view === 'monitoring' ? (
        <>
          {activeViewId != null ? (
            <div className="banner">
              Filtered to view: <strong>{customViews.find((v) => v.view_id === activeViewId)?.name ?? 'Unknown'}</strong>
              <button className="text-button" style={{ marginLeft: '1rem' }} onClick={() => setActiveViewId(null)}>Clear</button>
            </div>
          ) : null}
          <MonitoringTable
            agents={activeViewId != null
              ? agents.filter((a) => customViews.find((v) => v.view_id === activeViewId)?.agent_ids.includes(a.id) ?? false)
              : agents}
            error={error}
            expanded={expanded}
            groupFilter={overviewGroupFilter}
            groups={groups}
            global={global}
            loading={loading}
            setExpanded={setExpanded}
            setGroupFilter={setOverviewGroupFilter}
          />
        </>
      ) : null}
      {view === 'views' ? (
        <ViewsPage
          agents={agents}
          busyAction={busyAction}
          operator={operator}
          runAction={runAction}
          session={session}
          views={customViews}
          onOpenView={(viewId) => { setActiveViewId(viewId); setView('monitoring'); }}
          groups={groups}
        />
      ) : null}
      {view === 'agents' ? (
        <AgentManagement
          agents={agents}
          busyAction={busyAction}
          error={error}
          groups={groups}
          loading={loading}
          operator={operator}
          onConfigure={setConfigAgentId}
          pending={global && operator ? pending : []}
          showPending={global && operator}
          runAction={runAction}
        />
      ) : null}
      {configAgent ? (
        <AgentConfigModal
          agent={configAgent}
          busy={busyAction === `${configAgent.id}:collector_config`}
          onClose={() => setConfigAgentId(null)}
          operator={operator && canManageAgent(configAgent)}
          global={global}
          groups={groups}
          runAction={runAction}
        />
      ) : null}
      {view === 'alerts' ? (
        <AlertsPage agents={agents} alerts={toDisplayAlerts(allAlerts, agents)} busyAction={busyAction} operator={operator} runAction={runAction} />
      ) : null}
      {view === 'maintenance' ? (
        <MaintenanceWindowsPage agents={agents} busyAction={busyAction} global={global} groups={groups} operator={operator} runAction={runAction} windows={maintenanceWindows} />
      ) : null}
      {view === 'silences' ? (
        <SilencesPage agents={agents} busyAction={busyAction} global={global} operator={operator} runAction={runAction} silences={silences} />
      ) : null}
      {view === 'users' ? <UsersPage busyAction={busyAction} groups={groups} runAction={runAction} session={session} users={users} /> : null}
      {view === 'settings' ? <SettingsPage runAction={runAction} /> : null}
    </>
  );
}

function LoginScreen({ onLogin }: { onLogin: (session: SessionInfo) => void }) {
  const [username, setUsername] = React.useState('');
  const [password, setPassword] = React.useState('');
  const [error, setError] = React.useState<string | null>(null);
  return (
    <main className="login-wrap">
      <form
        className="login-panel"
        onSubmit={(event) => {
          event.preventDefault();
          void login(username, password).then(onLogin).catch((err) => setError(err instanceof Error ? err.message : 'Login failed'));
        }}
      >
        <div className="logo login-logo">
          <span className="logo-dot" />
          THEWATCHER
        </div>
        {error ? <div className="banner error">Login failed: {error}</div> : null}
        <label>
          Username
          <input value={username} onChange={(event) => setUsername(event.target.value)} />
        </label>
        <label>
          Password
          <input type="password" value={password} onChange={(event) => setPassword(event.target.value)} />
        </label>
        <button className="text-button" type="submit">Sign in</button>
      </form>
    </main>
  );
}

function Tab({ view, target, setView, label }: { view: View; target: View; setView: (view: View) => void; label: string }) {
  return (
    <button className={view === target ? 'active' : ''} onClick={() => setView(target)}>
      {label}
    </button>
  );
}

function Sparkline({ data, width = 80, height = 24, color = 'var(--text)' }: { data: number[]; width?: number; height?: number; color?: string }) {
  if (data.length < 2) return <svg width={width} height={height} />;
  const min = Math.min(...data);
  const max = Math.max(...data);
  const range = max - min || 1;
  const step = width / (data.length - 1);
  const points = data.map((v, i) => `${i * step},${height - ((v - min) / range) * (height - 2) - 1}`).join(' ');
  return (
    <svg width={width} height={height} style={{ overflow: 'visible' }}>
      <polyline points={points} fill="none" stroke={color} strokeWidth="1.5" strokeLinejoin="round" strokeLinecap="round" />
    </svg>
  );
}

function MonitoringTable({
  agents,
  error,
  expanded,
  global,
  groupFilter,
  groups,
  loading,
  setExpanded,
  setGroupFilter,
}: {
  agents: DashboardAgent[];
  error: string | null;
  expanded: Set<string>;
  global: boolean;
  groupFilter: OverviewGroupFilter;
  groups: GroupRecord[];
  loading: boolean;
  setExpanded: React.Dispatch<React.SetStateAction<Set<string>>>;
  setGroupFilter: React.Dispatch<React.SetStateAction<OverviewGroupFilter>>;
}) {
  const [agentHistory, setAgentHistory] = React.useState<Record<string, MetricsSnapshot[]>>({});
  const [agentLogMatches, setAgentLogMatches] = React.useState<Record<string, LogMatchRecord[]>>({});
  const [historyAgent, setHistoryAgent] = React.useState<{ id: string; name: string } | null>(null);

  React.useEffect(() => {
    for (const id of expanded) {
      if (!agentHistory[id]) {
        void fetchMetricHistory(id, 20).then((snapshots) => {
          setAgentHistory((prev) => ({ ...prev, [id]: snapshots }));
        }).catch(() => undefined);
        void fetchLogMatches(id, 20).then((rows) => {
          setAgentLogMatches((prev) => ({ ...prev, [id]: rows }));
        }).catch(() => undefined);
      }
    }
  }, [expanded, agentHistory]);

  const overviewGroups = groupOverviewAgents(agents, groups, groupFilter);
  const visibleAgentCount = overviewGroups.reduce((count, group) => count + group.agents.length, 0);

  return (
    <main className="table-wrap">
      {error ? <div className="banner error">API error: {error}</div> : null}
      {loading ? <div className="banner">Loading dashboard data...</div> : null}
      {!loading && agents.length === 0 ? <div className="banner">No approved agents are reporting yet.</div> : null}
      {!loading && agents.length > 0 && visibleAgentCount === 0 ? <div className="banner">No agents match this group filter.</div> : null}

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

function StatusHistoryModal({
  agentId,
  agentName,
  onClose,
}: {
  agentId: string;
  agentName: string;
  onClose: () => void;
}) {
  const [rows, setRows] = React.useState<StatusHistoryRow[] | null>(null);
  const [error, setError] = React.useState<string | null>(null);

  React.useEffect(() => {
    let cancelled = false;
    void fetchAgentHistory(agentId, 200)
      .then((data) => { if (!cancelled) setRows(data); })
      .catch((err) => { if (!cancelled) setError(err instanceof Error ? err.message : 'Failed to load status history'); });
    return () => { cancelled = true; };
  }, [agentId]);

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal-panel status-history-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <h2>Status History — {agentName}</h2>
          <button className="icon-button" onClick={onClose} aria-label="Close"><X size={14} /></button>
        </div>
        <div className="status-history-body">
          {error ? <div className="banner error">{error}</div> : null}
          {!error && rows === null ? <div className="banner">Loading status history…</div> : null}
          {rows !== null && rows.length === 0 ? <div className="banner">No status history recorded for this agent.</div> : null}
          {rows !== null && rows.length > 0 ? (
            <table className="history-table">
              <thead>
                <tr>
                  <th>Timestamp</th>
                  <th>Indicator</th>
                  <th colSpan={3}>Change</th>
                  <th>Message</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((row) => (
                  <tr key={row.id}>
                    <td className="history-ts">{new Date(row.created_at).toLocaleString()}</td>
                    <td>{row.indicator}</td>
                    <td><span className={`dot ${row.old_status} small-dot`} /></td>
                    <td>→</td>
                    <td><span className={`dot ${row.new_status} small-dot`} /></td>
                    <td className="history-msg">{row.message}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          ) : null}
        </div>
      </div>
    </div>
  );
}

function AgentManagement({
  agents,
  busyAction,
  error,
  groups,
  loading,
  onConfigure,
  operator,
  pending,
  showPending,
  runAction,
}: {
  agents: DashboardAgent[];
  busyAction: string | null;
  error: string | null;
  groups: GroupRecord[];
  loading: boolean;
  onConfigure: (agentId: string) => void;
  operator: boolean;
  pending: DashboardAgent[];
  showPending: boolean;
  runAction: RunAction;
}) {
  const [descDrafts, setDescDrafts] = React.useState<Record<string, string>>({});

  return (
    <main className="table-wrap management-wrap">
      {error ? <div className="banner error">API error: {error}</div> : null}
      {loading ? <div className="banner">Loading dashboard data...</div> : null}
      {showPending ? <PendingEnrollments agents={pending} busyAction={busyAction} groups={groups} runAction={runAction} /> : null}
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

function buildCollectorDraft(agent: DashboardAgent): AgentCollectorConfigUpdate {
  const config = collectorConfigWithDefaults(agent.collectorConfig);
  const diskByMount = new Map(config.disks.map((disk) => [disk.mount_point, disk]));
  const metricDisks =
    agent.metrics?.disks
      .filter((disk) => !diskByMount.has(disk.mount_point))
      .map<DiskMonitorConfig>((disk) => ({
        mount_point: disk.mount_point,
        device: disk.device,
        enabled: true,
        thresholds: { ...DEFAULT_PERCENT_THRESHOLDS },
        anomaly: { ...DEFAULT_ANOMALY_CONFIG },
      })) ?? [];
  const networkByName = new Map(config.networks.map((network) => [network.interface_name, network]));
  const metricNetworks =
    agent.metrics?.networks
      .filter((network) => network.interface_name !== 'lo' && !networkByName.has(network.interface_name))
      .map<NetworkInterfaceConfig>((network) => ({
        interface_name: network.interface_name,
        enabled: true,
        thresholds: { ...DEFAULT_NETWORK_THRESHOLDS },
        anomaly: { ...DEFAULT_ANOMALY_CONFIG },
      })) ?? [];

  return {
    collection_interval: agent.collectionInterval,
    heartbeat_interval: agent.heartbeatInterval,
    process_limit: agent.processLimit,
    group_ids: agent.groupIds,
    runbook_markdown: agent.runbookMarkdown,
    collector_config: {
      ...config,
      cpu: { ...config.cpu },
      memory: { ...config.memory },
      disks: [
        ...config.disks.map((disk) => ({
          ...disk,
          thresholds: { ...disk.thresholds },
          anomaly: { ...DEFAULT_ANOMALY_CONFIG, ...(disk.anomaly ?? {}) },
        })),
        ...metricDisks,
      ],
      networks: [
        ...config.networks.map((network) => ({
          ...network,
          thresholds: { ...network.thresholds },
          anomaly: { ...DEFAULT_ANOMALY_CONFIG, ...(network.anomaly ?? {}) },
        })),
        ...metricNetworks,
      ],
      processes: config.processes.map((process) => ({ ...process })),
      logs: (config.logs ?? []).map((log) => ({ ...log })),
    },
  };
}

function AgentConfigModal({
  agent: agentProp,
  busy,
  global,
  groups,
  onClose,
  operator,
  runAction,
}: {
  agent: DashboardAgent;
  busy: boolean;
  global: boolean;
  groups: GroupRecord[];
  onClose: () => void;
  operator: boolean;
  runAction: RunAction;
}) {
  // Snapshot the agent locally. The parent polls every 5s and rebuilds
  // `agents` with new object references, which would otherwise re-render
  // the form on every poll and risk fighting controlled-input state.
  // We only swap the snapshot when the *selected* agent id changes — i.e.
  // when the user opens a different agent's modal.
  const [agent, setAgent] = React.useState(agentProp);
  React.useEffect(() => {
    if (agentProp.id !== agent.id) setAgent(agentProp);
  }, [agentProp, agent.id]);

  const [draft, setDraft] = React.useState<AgentCollectorConfigUpdate>(() => buildCollectorDraft(agent));
  React.useEffect(() => {
    setDraft(buildCollectorDraft(agent));
  }, [agent]);

  const updateConfig = (update: (config: CollectorConfig) => CollectorConfig) => {
    setDraft((current) => ({ ...current, collector_config: update(current.collector_config) }));
  };
  const updatePercent = (collector: 'cpu' | 'memory', field: keyof typeof DEFAULT_PERCENT_THRESHOLDS, value: number) => {
    updateConfig((config) => ({
      ...config,
      [collector]: { ...config[collector], [field]: value },
    }));
  };
  const updateReading = (
    field: 'cpu_readings' | 'memory_readings' | 'disk_readings' | 'network_readings' | 'process_readings',
    value: number,
  ) => {
    updateConfig((config) => ({ ...config, [field]: value }));
  };
  const updateDisk = (index: number, update: (disk: DiskMonitorConfig) => DiskMonitorConfig) => {
    updateConfig((config) => ({
      ...config,
      disks: config.disks.map((disk, currentIndex) => (currentIndex === index ? update(disk) : disk)),
    }));
  };
  const updateNetwork = (index: number, update: (network: NetworkInterfaceConfig) => NetworkInterfaceConfig) => {
    updateConfig((config) => ({
      ...config,
      networks: config.networks.map((network, currentIndex) => (currentIndex === index ? update(network) : network)),
    }));
  };
  const updateProcess = (index: number, update: (process: ProcessWatchConfig) => ProcessWatchConfig) => {
    updateConfig((config) => ({
      ...config,
      processes: config.processes.map((process, currentIndex) => (currentIndex === index ? update(process) : process)),
    }));
  };
  const updateLog = (index: number, update: (log: LogMonitorConfig) => LogMonitorConfig) => {
    updateConfig((config) => ({
      ...config,
      logs: (config.logs ?? []).map((log, currentIndex) => (currentIndex === index ? update(log) : log)),
    }));
  };

  return (
    <div className="modal-backdrop" role="presentation">
      <form
        className="config-modal"
        onSubmit={(event) => {
          event.preventDefault();
          void runAction(`${agent.id}:collector_config`, () => setAgentCollectorConfig(agent.id, draft)).then((saved) => {
            if (saved) onClose();
          });
        }}
      >
        <div className="modal-header">
          <AgentIdentity id={agent.id} name={agent.name} />
          <button className="icon-button" type="button" onClick={onClose} aria-label="Close configuration">
            <X size={14} />
          </button>
        </div>

        <div className="config-grid">
          <section>
            <h2>Access</h2>
            <div className="form-grid">
              <label title="Controls which group-scoped users can see alerts, metrics, maintenance, and views for this agent.">
                Group
                <select
                  disabled={!global}
                  value={String(draft.group_ids?.[0] ?? 0)}
                  onChange={(event) => setDraft((current) => ({ ...current, group_ids: Number(event.target.value) > 0 ? [Number(event.target.value)] : [] }))}
                >
                  <option value={0}>No group</option>
                  {groups.map((group) => (
                    <option key={group.group_id} value={group.group_id}>{group.name}</option>
                  ))}
                </select>
              </label>
            </div>
          </section>

          <section>
            <h2>Collection</h2>
            <div className="form-grid">
              <NumberField help="How often collectors such as disk, CPU, memory, network, process, and logs run." label="Collection interval seconds" min={1} value={draft.collection_interval} onChange={(value) => setDraft((current) => ({ ...current, collection_interval: value }))} />
              <NumberField help="How often the agent sends an online heartbeat independent of collector sampling." label="Heartbeat seconds" min={1} max={60} value={draft.heartbeat_interval} onChange={(value) => setDraft((current) => ({ ...current, heartbeat_interval: value }))} />
              <NumberField help="Maximum number of top processes to include in each metric snapshot." label="Process sample limit" min={1} value={draft.process_limit} onChange={(value) => setDraft((current) => ({ ...current, process_limit: value }))} />
              <NumberField help="Consecutive CPU samples required before status changes." label="CPU readings" min={1} value={draft.collector_config.cpu_readings} onChange={(value) => updateReading('cpu_readings', value)} />
              <NumberField help="Consecutive memory samples required before status changes." label="Memory readings" min={1} value={draft.collector_config.memory_readings} onChange={(value) => updateReading('memory_readings', value)} />
              <NumberField help="Consecutive disk samples required before status changes." label="Disk readings" min={1} value={draft.collector_config.disk_readings} onChange={(value) => updateReading('disk_readings', value)} />
              <NumberField help="Consecutive network samples required before status changes." label="Network readings" min={1} value={draft.collector_config.network_readings} onChange={(value) => updateReading('network_readings', value)} />
              <NumberField help="Consecutive process-watch samples required before status changes." label="Process readings" min={1} value={draft.collector_config.process_readings} onChange={(value) => updateReading('process_readings', value)} />
              <NumberField help="Raises stale-data status when a metric value does not change for this many seconds. Zero disables it." label="Stale metric after (seconds, 0=off)" min={0} value={draft.collector_config.stale_after_seconds ?? 0} onChange={(value) => updateConfig((config) => ({ ...config, stale_after_seconds: value }))} />
            </div>
          </section>

          <section className="wide-section">
            <h2>Runbook</h2>
            <textarea
              className="runbook-editor"
              disabled={!operator}
              rows={10}
              title="Markdown instructions for this host. Include alarm-specific steps and links to external pages as needed."
              value={draft.runbook_markdown ?? ''}
              onChange={(event) => setDraft((current) => ({ ...current, runbook_markdown: event.target.value }))}
            />
          </section>

          <section>
            <h2>CPU & Memory</h2>
            <ThresholdRow label="CPU %" thresholds={draft.collector_config.cpu} onChange={(field, value) => updatePercent('cpu', field, value)} />
            <AnomalyInputs
              anomaly={draft.collector_config.cpu_anomaly ?? DEFAULT_ANOMALY_CONFIG}
              onChange={(field, value) => updateConfig((config) => ({ ...config, cpu_anomaly: { ...(config.cpu_anomaly ?? DEFAULT_ANOMALY_CONFIG), [field]: value } }))}
            />
            <ThresholdRow label="Memory %" thresholds={draft.collector_config.memory} onChange={(field, value) => updatePercent('memory', field, value)} />
            <AnomalyInputs
              anomaly={draft.collector_config.memory_anomaly ?? DEFAULT_ANOMALY_CONFIG}
              onChange={(field, value) => updateConfig((config) => ({ ...config, memory_anomaly: { ...(config.memory_anomaly ?? DEFAULT_ANOMALY_CONFIG), [field]: value } }))}
            />
          </section>

          <section className="wide-section">
            <h2>Fixed Disks</h2>
            <div className="config-list">
              {draft.collector_config.disks.map((disk, index) => (
                <div className="config-row" key={disk.mount_point}>
                  <label className="toggle-line" title="Enables or disables monitoring for this disk without removing its saved thresholds.">
                    <input type="checkbox" checked={disk.enabled} onChange={(event) => updateDisk(index, (current) => ({ ...current, enabled: event.target.checked }))} />
                    <span>{disk.device ? `${disk.mount_point} (${disk.device})` : disk.mount_point}</span>
                  </label>
                  <ThresholdInputs
                    thresholds={disk.thresholds}
                    onChange={(field, value) => updateDisk(index, (current) => ({ ...current, thresholds: { ...current.thresholds, [field]: value } }))}
                  />
                  <AnomalyInputs
                    anomaly={disk.anomaly ?? DEFAULT_ANOMALY_CONFIG}
                    onChange={(field, value) => updateDisk(index, (current) => ({ ...current, anomaly: { ...(current.anomaly ?? DEFAULT_ANOMALY_CONFIG), [field]: value } }))}
                  />
                </div>
              ))}
              {draft.collector_config.disks.length === 0 ? <div className="empty-config">No fixed disks reported yet.</div> : null}
            </div>
          </section>

          <section className="wide-section">
            <h2>Network Interfaces</h2>
            <div className="config-list">
              {draft.collector_config.networks.map((network, index) => (
                <div className="config-row" key={network.interface_name}>
                  <label className="toggle-line" title="Enables or disables monitoring for this network interface without removing its saved thresholds.">
                    <input type="checkbox" checked={network.enabled} onChange={(event) => updateNetwork(index, (current) => ({ ...current, enabled: event.target.checked }))} />
                    <span>{network.interface_name}</span>
                  </label>
                  <NetworkThresholdInputs
                    thresholds={network.thresholds}
                    onChange={(field, value) => updateNetwork(index, (current) => ({ ...current, thresholds: { ...current.thresholds, [field]: value } }))}
                  />
                  <AnomalyInputs
                    anomaly={network.anomaly ?? DEFAULT_ANOMALY_CONFIG}
                    onChange={(field, value) => updateNetwork(index, (current) => ({ ...current, anomaly: { ...(current.anomaly ?? DEFAULT_ANOMALY_CONFIG), [field]: value } }))}
                  />
                </div>
              ))}
              {draft.collector_config.networks.length === 0 ? <div className="empty-config">No network interfaces reported yet.</div> : null}
            </div>
          </section>

          <section className="wide-section">
            <div className="section-title-row">
              <h2>Process Watches</h2>
              <ActionButton
                busy={false}
                icon={<Plus size={14} />}
                label="Process"
                onClick={() =>
                  updateConfig((config) => ({
                    ...config,
                    processes: [...config.processes, { name: '', expected_count: 1, enabled: true }],
                  }))
                }
              />
            </div>
            <div className="config-list">
              {draft.collector_config.processes.map((process, index) => (
                // NOTE: do not include `process.name` in the key — the name
                // is being edited via the input below, so changing it would
                // change the key, forcing React to unmount/remount the row
                // and dropping the input's focus on every keystroke.
                <div className="process-row" key={`process:${index}`}>
                  <label className="toggle-line" title="Enables or disables this process watch without deleting it.">
                    <input type="checkbox" checked={process.enabled} onChange={(event) => updateProcess(index, (current) => ({ ...current, enabled: event.target.checked }))} />
                    <span>Enabled</span>
                  </label>
                  <input title="Exact executable or process name expected to be running on this agent." value={process.name} placeholder="exact executable name" onChange={(event) => updateProcess(index, (current) => ({ ...current, name: event.target.value }))} />
                  <NumberField help="Expected number of matching processes. Alerts are raised when fewer are running." label="Expected" min={1} value={process.expected_count} onChange={(value) => updateProcess(index, (current) => ({ ...current, expected_count: value }))} />
                  <ActionButton busy={false} danger icon={<Trash2 size={14} />} label="Remove" onClick={() => updateConfig((config) => ({ ...config, processes: config.processes.filter((_, currentIndex) => currentIndex !== index) }))} />
                </div>
              ))}
              {draft.collector_config.processes.length === 0 ? <div className="empty-config">No process watches configured.</div> : null}
            </div>
          </section>

          <section className="wide-section">
            <div className="section-title-row">
              <h2>Log Watches</h2>
              <ActionButton
                busy={false}
                icon={<Plus size={14} />}
                label="Log Watch"
                onClick={() =>
                  updateConfig((config) => ({
                    ...config,
                    logs: [...(config.logs ?? []), { path: '', pattern: '', indicator_name: '', severity: 'red', enabled: true }],
                  }))
                }
              />
            </div>
            <div className="config-list">
              {(draft.collector_config.logs ?? []).map((log, index) => (
                <div className="process-row" key={`log:${index}`}>
                  <label className="toggle-line" title="Enables or disables this log watch without deleting it.">
                    <input type="checkbox" checked={log.enabled} onChange={(event) => updateLog(index, (current) => ({ ...current, enabled: event.target.checked }))} />
                    <span>Enabled</span>
                  </label>
                  <input title="Absolute path to the log file the agent should scan." value={log.path} placeholder="/var/log/syslog" onChange={(event) => updateLog(index, (current) => ({ ...current, path: event.target.value }))} />
                  <input title="Regular expression pattern that identifies matching log lines." value={log.pattern} placeholder="regex pattern" onChange={(event) => updateLog(index, (current) => ({ ...current, pattern: event.target.value }))} />
                  <input title="Dashboard indicator name to use when this log pattern matches." value={log.indicator_name} placeholder="indicator name" onChange={(event) => updateLog(index, (current) => ({ ...current, indicator_name: event.target.value }))} />
                  <select title="Alert severity raised when this log pattern matches." value={log.severity} onChange={(event) => updateLog(index, (current) => ({ ...current, severity: event.target.value as LogMonitorConfig['severity'] }))}>
                    <option value="yellow">Warning</option>
                    <option value="amber">Degraded</option>
                    <option value="red">Critical</option>
                  </select>
                  <ActionButton busy={false} danger icon={<Trash2 size={14} />} label="Remove" onClick={() => updateConfig((config) => ({ ...config, logs: (config.logs ?? []).filter((_, currentIndex) => currentIndex !== index) }))} />
                </div>
              ))}
              {(draft.collector_config.logs ?? []).length === 0 ? <div className="empty-config">No log watches configured.</div> : null}
            </div>
          </section>
        </div>

        <div className="modal-actions">
          <button className="text-button" type="button" onClick={onClose}>Cancel</button>
          <button className="text-button" type="submit" disabled={!operator || busy}>{busy ? '...' : 'Save'}</button>
        </div>
      </form>
    </div>
  );
}

function NumberField({
  help,
  label,
  max,
  min,
  onChange,
  step = 1,
  value,
}: {
  help?: string;
  label: string;
  max?: number;
  min: number;
  onChange: (value: number) => void;
  step?: number;
  value: number;
}) {
  return (
    <label title={help}>
      {label}
      <input max={max} min={min} step={step} type="number" value={value} onChange={(event) => onChange(Number(event.target.value))} />
    </label>
  );
}

function AnomalyInputs({
  anomaly,
  onChange,
}: {
  anomaly: AnomalyConfig;
  onChange: (field: keyof AnomalyConfig, value: number) => void;
}) {
  return (
    <div className="anomaly-inputs">
      <NumberField help="Multiplier above the learned baseline required before anomaly status is raised. Zero disables anomaly detection." label="Anomaly multiplier (0=off)" min={0} step={0.1} value={anomaly.multiplier} onChange={(value) => onChange('multiplier', value)} />
      <NumberField help="Number of hours of recent samples used to calculate the normal baseline for anomaly detection." label="Baseline hours" min={1} value={anomaly.baseline_window_hours} onChange={(value) => onChange('baseline_window_hours', value)} />
    </div>
  );
}

function ThresholdInputs({
  onChange,
  thresholds,
}: {
  onChange: (field: keyof typeof DEFAULT_PERCENT_THRESHOLDS, value: number) => void;
  thresholds: typeof DEFAULT_PERCENT_THRESHOLDS;
}) {
  return (
    <div className="threshold-inputs">
      <NumberField help="Usage percent that raises warning status for this item." label="Warn" min={1} value={thresholds.warning_percent} onChange={(value) => onChange('warning_percent', value)} />
      <NumberField help="Usage percent that raises degraded status for this item." label="Degrade" min={1} value={thresholds.degraded_percent} onChange={(value) => onChange('degraded_percent', value)} />
      <NumberField help="Usage percent that raises critical status for this item." label="Critical" min={1} value={thresholds.critical_percent} onChange={(value) => onChange('critical_percent', value)} />
    </div>
  );
}

function ThresholdRow({
  label,
  onChange,
  thresholds,
}: {
  label: string;
  onChange: (field: keyof typeof DEFAULT_PERCENT_THRESHOLDS, value: number) => void;
  thresholds: typeof DEFAULT_PERCENT_THRESHOLDS;
}) {
  return (
    <div className="threshold-row">
      <span>{label}</span>
      <ThresholdInputs thresholds={thresholds} onChange={onChange} />
    </div>
  );
}

function NetworkThresholdInputs({
  onChange,
  thresholds,
}: {
  onChange: (field: keyof typeof DEFAULT_NETWORK_THRESHOLDS, value: number) => void;
  thresholds: typeof DEFAULT_NETWORK_THRESHOLDS;
}) {
  return (
    <div className="threshold-inputs">
      <NumberField help="Network throughput in megabits per second that raises warning status." label="Warn Mb/s" min={1} value={thresholds.warning_mbps} onChange={(value) => onChange('warning_mbps', value)} />
      <NumberField help="Network throughput in megabits per second that raises degraded status." label="Degrade Mb/s" min={1} value={thresholds.degraded_mbps} onChange={(value) => onChange('degraded_mbps', value)} />
      <NumberField help="Network throughput in megabits per second that raises critical status." label="Critical Mb/s" min={1} value={thresholds.critical_mbps} onChange={(value) => onChange('critical_mbps', value)} />
    </div>
  );
}

function PendingEnrollments({ agents, busyAction, groups, runAction }: { agents: DashboardAgent[]; busyAction: string | null; groups: GroupRecord[]; runAction: RunAction }) {
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

function AcknowledgeModal({ onClose, onSubmit, bulk }: { onClose: () => void; onSubmit: (note: string, andArchive: boolean) => void; bulk: boolean }) {
  const [note, setNote] = React.useState('');
  const [andArchive, setAndArchive] = React.useState(false);
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal-panel" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <h2>{bulk ? 'Acknowledge selected alerts' : 'Acknowledge alert'}</h2>
          <button className="icon-button" onClick={onClose}><X size={14} /></button>
        </div>
        <label>
          Note <span className="field-hint">(optional)</span>
          <textarea
            className="note-input"
            rows={3}
            placeholder="Describe what you found or what action was taken…"
            value={note}
            onChange={(e) => setNote(e.target.value)}
            autoFocus
          />
        </label>
        <label className="checkbox-label">
          <input type="checkbox" checked={andArchive} onChange={(e) => setAndArchive(e.target.checked)} />
          Also archive after acknowledging
        </label>
        <div className="button-row modal-actions">
          <button className="text-button secondary" onClick={onClose}>Cancel</button>
          <button className="text-button" onClick={() => onSubmit(note, andArchive)}>
            {andArchive ? 'Acknowledge & Archive' : 'Acknowledge'}
          </button>
        </div>
      </div>
    </div>
  );
}

function renderInlineMarkdown(text: string): React.ReactNode[] {
  const nodes: React.ReactNode[] = [];
  const linkPattern = /\[([^\]]+)]\(([^)]+)\)/g;
  let lastIndex = 0;
  let match: RegExpExecArray | null;
  while ((match = linkPattern.exec(text)) !== null) {
    if (match.index > lastIndex) nodes.push(text.slice(lastIndex, match.index));
    const href = safeMarkdownHref(match[2]);
    nodes.push(
      href ? (
        <a href={href} key={`${match.index}:${href}`} rel="noopener noreferrer" target="_blank">
          {match[1]}
        </a>
      ) : (
        match[1]
      ),
    );
    lastIndex = match.index + match[0].length;
  }
  if (lastIndex < text.length) nodes.push(text.slice(lastIndex));
  return nodes;
}

function RunbookMarkdown({ markdown }: { markdown: string }) {
  const blocks = parseMarkdownBlocks(markdown);
  if (blocks.length === 0) return <div className="banner">No runbook has been configured for this agent.</div>;
  return (
    <div className="runbook-markdown">
      {blocks.map((block, index) => {
        if (block.type === 'heading') {
          const Heading = `h${block.level}` as 'h1' | 'h2' | 'h3';
          return <Heading key={index}>{renderInlineMarkdown(block.text)}</Heading>;
        }
        if (block.type === 'list') {
          return (
            <ul key={index}>
              {block.items.map((item, itemIndex) => <li key={itemIndex}>{renderInlineMarkdown(item)}</li>)}
            </ul>
          );
        }
        if (block.type === 'code') return <pre key={index}><code>{block.text}</code></pre>;
        return <p key={index}>{renderInlineMarkdown(block.text)}</p>;
      })}
    </div>
  );
}

function RunbookModal({ agentName, markdown, onClose }: { agentName: string; markdown: string; onClose: () => void }) {
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal-panel runbook-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-header">
          <h2>Runbook - {agentName}</h2>
          <button className="icon-button" onClick={onClose} aria-label="Close runbook"><X size={14} /></button>
        </div>
        <div className="runbook-body">
          <RunbookMarkdown markdown={markdown} />
        </div>
      </div>
    </div>
  );
}

function AlertsPage({
  agents,
  alerts,
  busyAction,
  operator,
  runAction,
}: {
  agents: DashboardAgent[];
  alerts: ReturnType<typeof toDisplayAlerts>;
  busyAction: string | null;
  operator: boolean;
  runAction: RunAction;
}) {
  const [selected, setSelected] = React.useState<Set<number>>(new Set());
  const [showArchived, setShowArchived] = React.useState(false);
  const [archivedAlerts, setArchivedAlerts] = React.useState<ReturnType<typeof toDisplayAlerts>>([]);
  const [agentFilter, setAgentFilter] = React.useState('');
  const [statusFilter, setStatusFilter] = React.useState('');
  const [ackTarget, setAckTarget] = React.useState<number | 'bulk' | null>(null);
  const [runbookTarget, setRunbookTarget] = React.useState<{ agentName: string; markdown: string } | null>(null);

  // Re-fetch archived alerts whenever the live alerts list refreshes (proxy for parent refresh)
  React.useEffect(() => {
    if (!showArchived) return;
    void fetchAlerts(true).then((all) => setArchivedAlerts(toDisplayAlerts(all, agents)));
  }, [showArchived, alerts, agents]);

  const baseAlerts = showArchived ? archivedAlerts : alerts;
  const displayed = baseAlerts.filter((a) => {
    if (agentFilter && a.agentId !== agentFilter) return false;
    if (statusFilter && a.new_status !== statusFilter) return false;
    return true;
  });

  const displayedIds = displayed.map((a) => a.alert_id);
  const allSelected = displayedIds.length > 0 && displayedIds.every((id) => selected.has(id));

  const toggleSelect = (id: number) =>
    setSelected((prev) => { const n = new Set(prev); n.has(id) ? n.delete(id) : n.add(id); return n; });
  const toggleSelectAll = () =>
    setSelected(allSelected ? new Set() : new Set(displayedIds));

  const afterAction = () => setSelected(new Set());

  const handleAckSubmit = async (note: string, andArchive: boolean) => {
    setAckTarget(null);
    if (ackTarget === 'bulk') {
      const ids = [...selected];
      await runAction('bulk:ack', async () => {
        await bulkAcknowledgeAlerts(ids, note);
        if (andArchive) await bulkArchiveAlerts(ids);
      });
    } else if (ackTarget !== null) {
      const id = ackTarget;
      await runAction(`alert:${id}:ack`, async () => {
        await acknowledgeAlert(id, note);
        if (andArchive) await archiveAlert(id);
      });
    }
    afterAction();
  };

  const uniqueAgents = [...new Map(alerts.map((a) => [a.agentId, a.agentName])).entries()];
  const selectedCount = selected.size;

  return (
    <main className="table-wrap management-wrap">
      {ackTarget !== null ? (
        <AcknowledgeModal bulk={ackTarget === 'bulk'} onClose={() => setAckTarget(null)} onSubmit={handleAckSubmit} />
      ) : null}
      {runbookTarget ? (
        <RunbookModal
          agentName={runbookTarget.agentName}
          markdown={runbookTarget.markdown}
          onClose={() => setRunbookTarget(null)}
        />
      ) : null}
      <div className="management-header">
        <h1>Alerts</h1>
        <div className="header-controls">
          <select value={agentFilter} onChange={(e) => setAgentFilter(e.target.value)}>
            <option value="">All agents</option>
            {uniqueAgents.map(([id, name]) => <option key={id} value={id}>{name}</option>)}
          </select>
          <select value={statusFilter} onChange={(e) => setStatusFilter(e.target.value)}>
            <option value="">All statuses</option>
            <option value="red">Critical</option>
            <option value="amber">Degraded</option>
            <option value="yellow">Warning</option>
            <option value="green">Healthy</option>
          </select>
          <label className="checkbox-label">
            <input type="checkbox" checked={showArchived} onChange={(e) => setShowArchived(e.target.checked)} />
            Show archived
          </label>
        </div>
      </div>
      {selectedCount > 0 && operator ? (
        <div className="bulk-action-bar">
          <span>{selectedCount} selected</span>
          <div className="button-row">
            <ActionButton busy={busyAction === 'bulk:ack'} icon={<Check size={14} />} label={`Acknowledge (${selectedCount})`} onClick={() => setAckTarget('bulk')} />
            <ActionButton busy={busyAction === 'bulk:archive'} danger icon={<Archive size={14} />} label={`Archive (${selectedCount})`} onClick={() => runAction('bulk:archive', async () => { await bulkArchiveAlerts([...selected]); afterAction(); })} />
          </div>
        </div>
      ) : null}
      <div className="table-container">
        <table className="management-table">
          <thead>
            <tr>
              <th className="col-check">
                <button className="icon-button" onClick={toggleSelectAll} disabled={displayedIds.length === 0}>
                  {allSelected ? <CheckSquare size={14} /> : <Square size={14} />}
                </button>
              </th>
              <th>Agent</th><th>Indicator</th><th>State</th><th>Created</th><th>Message</th><th>Acknowledged</th><th>Actions</th>
            </tr>
          </thead>
          <tbody>{displayed.map((alert) => {
            const isAcknowledged = alert.acknowledged_at > 0;
            const isArchived = alert.deleted_at > 0;
            const agent = agents.find((candidate) => candidate.id === alert.agentId);
            const runbookMarkdown = alert.runbook_markdown || agent?.runbookMarkdown || '';
            return (
              <tr key={alert.alert_id} className={isArchived ? 'row-archived' : ''}>
                <td className="col-check">
                  <button className="icon-button" onClick={() => toggleSelect(alert.alert_id)}>
                    {selected.has(alert.alert_id) ? <CheckSquare size={14} /> : <Square size={14} />}
                  </button>
                </td>
                <td><AgentIdentity id={alert.agentId} name={alert.agentName} /></td>
                <td>{alert.indicator}</td>
                <td><StatusDot color={alert.new_status} label={colorLabel(alert.new_status)} acknowledged={isAcknowledged} /></td>
                <td className="uptime">{new Date(alert.created_at).toLocaleString()}</td>
                <td>
                  {alert.message}
                  {runbookMarkdown.trim() ? (
                    <button
                      className="runbook-link"
                      onClick={() => setRunbookTarget({ agentName: alert.agentName, markdown: runbookMarkdown })}
                      type="button"
                    >
                      <BookOpen size={12} /> Runbook
                    </button>
                  ) : null}
                </td>
                <td className="uptime">
                  {isAcknowledged ? (
                    <span>
                      {alert.acknowledged_by} &mdash; {new Date(alert.acknowledged_at).toLocaleString()}
                      {alert.note ? <em className="alert-note"> — {alert.note}</em> : null}
                    </span>
                  ) : alert.escalated_at > 0 ? (
                    <span className="escalated-badge">ESCALATED</span>
                  ) : null}
                </td>
                <td>
                  {!isArchived ? (
                    <div className="button-row">
                      {!isAcknowledged ? (
                        <>
                          <ActionButton busy={busyAction === `alert:${alert.alert_id}:ack`} disabled={!operator} icon={<Check size={14} />} label="Acknowledge" onClick={() => setAckTarget(alert.alert_id)} />
                          <ActionButton busy={busyAction === `alert:${alert.alert_id}:ack-archive`} disabled={!operator} icon={<Archive size={14} />} label="Acknowledge & Archive" onClick={() => runAction(`alert:${alert.alert_id}:ack-archive`, async () => { await acknowledgeAlert(alert.alert_id); await archiveAlert(alert.alert_id); })} />
                        </>
                      ) : (
                        <ActionButton busy={busyAction === `alert:${alert.alert_id}:archive`} danger disabled={!operator} icon={<Archive size={14} />} label="Archive" onClick={() => runAction(`alert:${alert.alert_id}:archive`, () => archiveAlert(alert.alert_id))} />
                      )}
                    </div>
                  ) : <span className="uptime">Archived</span>}
                </td>
              </tr>
            );
          })}</tbody>
        </table>
      </div>
    </main>
  );
}

function MaintenanceWindowsPage({
  agents,
  busyAction,
  global,
  groups,
  operator,
  runAction,
  windows,
}: {
  agents: DashboardAgent[];
  busyAction: string | null;
  global: boolean;
  groups: GroupRecord[];
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
            void runAction('mw:create', () => createMaintenanceWindow(agentId, startMs, endMs, reason)).then(() => {
              setReason('');
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
              style={{ width: 60 }}
            />
          </label>
          <ActionButton busy={busyAction === 'mw:create'} disabled={!operator} icon={<Plus size={14} />} label="Schedule" onClick={() => undefined} type="submit" />
        </form>
      ) : null}
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
            {windows.length === 0 ? (
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

function SilencesPage({
  agents,
  busyAction,
  global,
  operator,
  runAction,
  silences,
}: {
  agents: DashboardAgent[];
  busyAction: string | null;
  global: boolean;
  operator: boolean;
  runAction: RunAction;
  silences: SilenceRecord[];
}) {
  const [agentId, setAgentId] = React.useState('*');
  const [indicator, setIndicator] = React.useState('*');
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
            void runAction('silence:create', () => createSilence(agentId, indicator, reason, untilMs)).then(() => {
              setReason('');
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
          <div className="button-row">
            <button className="text-button" disabled={busyAction !== null} type="submit">
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
            {silences.length === 0 ? (
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
                        busy={busyAction === `silence:delete:${s.silence_id}`}
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

function UsersPage({
  busyAction,
  groups,
  runAction,
  session,
  users,
}: {
  busyAction: string | null;
  groups: GroupRecord[];
  runAction: RunAction;
  session: SessionInfo;
  users: UserRecord[];
}) {
  const [groupName, setGroupName] = React.useState('');
  const [username, setUsername] = React.useState('');
  const [password, setPassword] = React.useState('');
  const [role, setRole] = React.useState<UserRecord['role']>('group_viewer');
  const [groupId, setGroupId] = React.useState<number>(groups[0]?.group_id ?? 0);
  const [pwUserId, setPwUserId] = React.useState<number | null>(null);
  const [pwValue, setPwValue] = React.useState('');

  const admin = canManageUsers(session.role);
  const globalAdmin = session.role === 'global_admin';
  const globalAccount = isGlobal(session.role);
  const currentUser = users.find((u) => u.username === session.username);

  React.useEffect(() => {
    if (!globalAccount && groups.length > 0 && !groups.some((group) => group.group_id === groupId)) {
      setGroupId(groups[0].group_id);
    }
  }, [globalAccount, groupId, groups]);

  return (
    <main className="table-wrap management-wrap">
      {pwUserId !== null ? (
        <div className="modal-backdrop" onClick={() => setPwUserId(null)}>
          <div className="modal-panel" onClick={(e) => e.stopPropagation()}>
            <div className="modal-header">
              <h2>Change Password</h2>
              <button className="icon-button" onClick={() => setPwUserId(null)}><X size={14} /></button>
            </div>
            <label>
              New password
              <input
                type="password"
                autoFocus
                value={pwValue}
                onChange={(e) => setPwValue(e.target.value)}
              />
            </label>
            <div className="button-row modal-actions">
              <button className="text-button secondary" onClick={() => setPwUserId(null)}>Cancel</button>
              <button
                className="text-button"
                disabled={!pwValue}
                onClick={() => {
                  const id = pwUserId;
                  const pw = pwValue;
                  setPwUserId(null);
                  setPwValue('');
                  void runAction(`user:${id}:pw`, () => changeUserPassword(id, pw));
                }}
              >
                Save
              </button>
            </div>
          </div>
        </div>
      ) : null}
      <div className="management-header"><h1>Users & Groups</h1></div>
      <div className="management-grid">
        {canManageGroups(session.role) ? <form
          className="inline-form"
          onSubmit={(event) => {
            event.preventDefault();
            if (!groupName.trim()) return;
            void runAction('group:create', () => createGroup(groupName.trim())).then(() => setGroupName(''));
          }}
        >
          <input aria-label="Group name" placeholder="Group name" value={groupName} onChange={(event) => setGroupName(event.target.value)} />
          <ActionButton busy={busyAction === 'group:create'} icon={<Plus size={14} />} label="Group" onClick={() => undefined} type="submit" />
        </form> : null}
        {admin ? <form
          className="inline-form user-form"
          onSubmit={(event) => {
            event.preventDefault();
            if (!username.trim() || !password) return;
            void runAction('user:create', () => createUser(username.trim(), password, role, groupId ? [groupId] : [])).then(() => {
              setUsername('');
              setPassword('');
            });
          }}
        >
          <input aria-label="Username" placeholder="Username" value={username} onChange={(event) => setUsername(event.target.value)} />
          <input aria-label="Password" placeholder="Password" type="password" value={password} onChange={(event) => setPassword(event.target.value)} />
          <select aria-label="Account type" value={role} onChange={(event) => setRole(event.target.value as UserRecord['role'])}>
            {globalAccount ? <option value="global_admin">{accountTypeLabel('global_admin')}</option> : null}
            {globalAccount ? <option value="global_operator">{accountTypeLabel('global_operator')}</option> : null}
            {globalAccount ? <option value="global_viewer">{accountTypeLabel('global_viewer')}</option> : null}
            <option value="group_admin">{accountTypeLabel('group_admin')}</option>
            <option value="group_operator">{accountTypeLabel('group_operator')}</option>
            <option value="group_viewer">{accountTypeLabel('group_viewer')}</option>
          </select>
          <select aria-label="Group" value={groupId} onChange={(event) => setGroupId(Number(event.target.value))}>
            {globalAccount ? <option value={0}>No group</option> : null}
            {groups.map((group) => (
              <option key={group.group_id} value={group.group_id}>{group.name}</option>
            ))}
          </select>
          <ActionButton busy={busyAction === 'user:create'} icon={<Plus size={14} />} label="User" onClick={() => undefined} type="submit" />
        </form> : null}
      </div>
      <div className="table-container">
        <table className="management-table">
          <thead><tr><th>User</th><th>Type</th><th>Groups</th><th>State</th><th>Actions</th></tr></thead>
          <tbody>{users.map((user) => {
            const canChangePw = admin || currentUser?.user_id === user.user_id;
            return (
              <tr key={user.user_id} className={user.disabled ? 'row-archived' : ''}>
                <td>{user.username}</td>
                <td>{accountTypeLabel(user.role)}</td>
                <td>{user.group_ids.map((id) => groups.find((group) => group.group_id === id)?.name ?? id).join(', ')}</td>
                <td>{user.disabled ? 'Disabled' : 'Active'}</td>
                <td>
                  <div className="button-row">
                    {canChangePw ? (
                      <ActionButton
                        busy={busyAction === `user:${user.user_id}:pw`}
                        icon={<SlidersHorizontal size={14} />}
                        label="Password"
                        onClick={() => { setPwUserId(user.user_id); setPwValue(''); }}
                      />
                    ) : null}
                    {globalAdmin && !user.built_in ? (
                      user.disabled ? (
                        <ActionButton
                          busy={busyAction === `user:${user.user_id}:enable`}
                          icon={<Play size={14} />}
                          label="Enable"
                          onClick={() => runAction(`user:${user.user_id}:enable`, () => enableUser(user.user_id))}
                        />
                      ) : (
                        <ActionButton
                          busy={busyAction === `user:${user.user_id}:disable`}
                          icon={<Pause size={14} />}
                          label="Disable"
                          onClick={() => runAction(`user:${user.user_id}:disable`, () => disableUser(user.user_id))}
                        />
                      )
                    ) : null}
                    {globalAdmin && !user.built_in ? (
                      <ActionButton
                        busy={busyAction === `user:${user.user_id}:delete`}
                        danger
                        icon={<Trash2 size={14} />}
                        label="Delete"
                        onClick={() => runAction(`user:${user.user_id}:delete`, () => deleteUser(user.user_id))}
                      />
                    ) : null}
                  </div>
                </td>
              </tr>
            );
          })}</tbody>
        </table>
      </div>
    </main>
  );
}

function RunbooksPage({
  runAction,
}: {
  runAction: RunAction;
}) {
  const [runbooks, setRunbooks] = React.useState<RunbookRecord[]>([]);
  const [indicator, setIndicator] = React.useState('*');
  const [status, setStatus] = React.useState<'yellow' | 'amber' | 'red'>('red');
  const [url, setUrl] = React.useState('');
  const [notes, setNotes] = React.useState('');

  const load = React.useCallback(() => {
    void fetchRunbooks().then(setRunbooks).catch(() => undefined);
  }, []);

  React.useEffect(() => { load(); }, [load]);

  return (
    <main className="table-wrap management-wrap">
      <div className="management-header"><h1>Runbooks</h1></div>
      <form
        className="settings-form"
        onSubmit={(event) => {
          event.preventDefault();
          void runAction('runbook:create', async () => {
            await createRunbook(indicator, status, url, notes);
            setUrl('');
            setNotes('');
            setIndicator('*');
            load();
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
        <div className="button-row">
          <button className="text-button" type="submit" disabled={!url}>
            <Plus size={14} /> Add Runbook
          </button>
        </div>
      </form>
      <div className="table-outer">
        <table className="agent-table">
          <thead><tr><th>Indicator</th><th>Status</th><th>URL</th><th>Notes</th><th>Created By</th><th>Actions</th></tr></thead>
          <tbody>
            {runbooks.length === 0 ? (
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
                    onClick={() => void runAction(`runbook:delete:${rb.runbook_id}`, async () => {
                      await deleteRunbook(rb.runbook_id);
                      load();
                    })}
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

function SettingsPage({
  runAction,
}: {
  runAction: RunAction;
}) {
  const [draft, setDraft] = React.useState<ServerSettings>({ webhook_url: '', offline_after_seconds: 120, escalation_timeout_seconds: 300, metrics_retention_days: 30, reports_enabled: false, reports_schedule: 'daily', reports_hour: 8, reports_day_of_week: 1, reports_webhook_url: '' });
  const [loaded, setLoaded] = React.useState(false);

  React.useEffect(() => {
    void fetchSettings().then((s) => {
      setDraft(s);
      setLoaded(true);
    }).catch(() => setLoaded(true));
  }, []);

  return (
    <main className="table-wrap management-wrap">
      <div className="management-header"><h1>Settings</h1></div>
      {!loaded ? <div className="banner">Loading settings...</div> : null}
      {loaded ? (
        <form
          className="settings-form"
          onSubmit={(event) => {
            event.preventDefault();
            void runAction('settings:save', () => updateSettings(draft));
          }}
        >
          <div className="form-grid">
            <label>
              Webhook URL
              <input
                placeholder="https://hooks.example.com/…"
                type="url"
                value={draft.webhook_url}
                onChange={(e) => setDraft((d) => ({ ...d, webhook_url: e.target.value }))}
              />
            </label>
            <label>
              Offline after (seconds)
              <input
                min={10}
                step={1}
                type="number"
                value={draft.offline_after_seconds}
                onChange={(e) => setDraft((d) => ({ ...d, offline_after_seconds: Number(e.target.value) }))}
              />
            </label>
            <label>
              Escalation timeout (seconds)
              <input
                min={60}
                step={1}
                type="number"
                value={draft.escalation_timeout_seconds}
                onChange={(e) => setDraft((d) => ({ ...d, escalation_timeout_seconds: Number(e.target.value) }))}
              />
            </label>
            <label>
              Metrics retention (days)
              <input
                min={1}
                max={365}
                step={1}
                type="number"
                value={draft.metrics_retention_days}
                onChange={(e) => setDraft((d) => ({ ...d, metrics_retention_days: Number(e.target.value) }))}
              />
            </label>
          </div>
          <h2>Scheduled Reports</h2>
          <div className="form-grid">
            <label>
              Enable digest reports
              <input
                type="checkbox"
                checked={draft.reports_enabled}
                onChange={(e) => setDraft((d) => ({ ...d, reports_enabled: e.target.checked }))}
              />
            </label>
            <label>
              Schedule
              <select
                value={draft.reports_schedule}
                onChange={(e) => setDraft((d) => ({ ...d, reports_schedule: e.target.value as 'daily' | 'weekly' }))}
              >
                <option value="daily">Daily</option>
                <option value="weekly">Weekly</option>
              </select>
            </label>
            <label>
              Hour (UTC, 0–23)
              <input
                min={0}
                max={23}
                step={1}
                type="number"
                value={draft.reports_hour}
                onChange={(e) => setDraft((d) => ({ ...d, reports_hour: Number(e.target.value) }))}
              />
            </label>
            {draft.reports_schedule === 'weekly' ? (
              <label>
                Day of week
                <select
                  value={draft.reports_day_of_week}
                  onChange={(e) => setDraft((d) => ({ ...d, reports_day_of_week: Number(e.target.value) }))}
                >
                  <option value={0}>Sunday</option>
                  <option value={1}>Monday</option>
                  <option value={2}>Tuesday</option>
                  <option value={3}>Wednesday</option>
                  <option value={4}>Thursday</option>
                  <option value={5}>Friday</option>
                  <option value={6}>Saturday</option>
                </select>
              </label>
            ) : null}
            <label>
              Report webhook URL
              <input
                placeholder="https://hooks.example.com/…"
                type="url"
                value={draft.reports_webhook_url}
                onChange={(e) => setDraft((d) => ({ ...d, reports_webhook_url: e.target.value }))}
              />
            </label>
          </div>
          <div className="button-row">
            <button className="text-button" type="submit">Save Settings</button>
            <button
              className="text-button"
              type="button"
              onClick={() => void runAction('reports:send', () => sendReport().then(() => undefined))}
            >
              Send Report Now
            </button>
          </div>
        </form>
      ) : null}
    </main>
  );
}

function AgentIdentity({ id, name, prefix, secondary }: { id: string; name: string; prefix?: React.ReactNode; secondary?: string }) {
  return (
    <div>
      <div className="server-name">
        {prefix}
        <span>{name || id}</span>
      </div>
      <div className="agent-id">{secondary ?? id}</div>
    </div>
  );
}

function StatusDot({ color, label, acknowledged = false }: { color: HealthColor; label: string; acknowledged?: boolean }) {
  return (
    <span className="state-note">
      <span className={`dot ${color}${acknowledged ? ' acknowledged' : ''}`} aria-label={acknowledged ? `${label} (acknowledged)` : undefined} />
      {' '}{label}
      {acknowledged ? <span className="acknowledged-tag" aria-hidden="true">ack</span> : null}
    </span>
  );
}

function ActionButton({ busy, danger = false, disabled = false, icon, label, onClick, type = 'button' }: { busy: boolean; danger?: boolean; disabled?: boolean; icon: React.ReactNode; label: string; onClick: () => void; type?: 'button' | 'submit' }) {
  return (
    <button className={`text-button ${danger ? 'danger' : ''}`} disabled={disabled || busy} onClick={onClick} type={type}>
      {icon}
      {busy ? '...' : label}
    </button>
  );
}

function SummaryCard({ label, value, color }: { label: string; value: number; color: HealthColor }) {
  return (
    <div className="summary-card">
      <div className="summary-label">{label}</div>
      <div className={`summary-value ${color}`}>{value}</div>
    </div>
  );
}

function ViewsPage({
  agents,
  busyAction,
  operator,
  runAction,
  session,
  views,
  onOpenView,
  groups,
}: {
  agents: DashboardAgent[];
  busyAction: string | null;
  operator: boolean;
  runAction: RunAction;
  session: SessionInfo;
  views: ViewRecord[];
  onOpenView: (viewId: number) => void;
  groups: GroupRecord[];
}) {
  const [newName, setNewName] = React.useState('');
  const [newPublic, setNewPublic] = React.useState(false);
  const [newGroupId, setNewGroupId] = React.useState<number>(groups[0]?.group_id ?? 0);
  const [newAgentIds, setNewAgentIds] = React.useState<string[]>([]);
  const [editId, setEditId] = React.useState<number | null>(null);
  const [editName, setEditName] = React.useState('');
  const [editPublic, setEditPublic] = React.useState(false);
  const [editGroupId, setEditGroupId] = React.useState(0);
  const [editAgentIds, setEditAgentIds] = React.useState<string[]>([]);
  const globalAccount = isGlobal(session.role);

  React.useEffect(() => {
    if (!globalAccount && groups.length > 0 && !groups.some((group) => group.group_id === newGroupId)) {
      setNewGroupId(groups[0].group_id);
    }
  }, [globalAccount, groups, newGroupId]);

  React.useEffect(() => {
    if (!globalAccount && editId !== null && groups.length > 0 && !groups.some((group) => group.group_id === editGroupId)) {
      setEditGroupId(groups[0].group_id);
    }
  }, [editGroupId, editId, globalAccount, groups]);

  const startEdit = (v: ViewRecord) => {
    setEditId(v.view_id);
    setEditName(v.name);
    setEditPublic(v.is_public);
    setEditGroupId(v.group_id);
    setEditAgentIds([...v.agent_ids]);
  };

  const canEdit = (v: ViewRecord) => operator && (v.owner_username === session.username || isGlobal(session.role));

  return (
    <main className="table-wrap">
      <div className="table-container">
        <table>
          <thead>
            <tr>
              <th>Name</th>
              <th>Owner</th>
              <th>Agents</th>
              <th>Visibility</th>
              <th>Group</th>
              <th>Created</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {views.map((v) => (
              <tr key={v.view_id}>
                {editId === v.view_id ? (
                  <>
                    <td colSpan={5}>
                      <div className="form-grid" style={{ display: 'flex', gap: '0.5rem', flexWrap: 'wrap', alignItems: 'center' }}>
                        <input value={editName} placeholder="View name" onChange={(e) => setEditName(e.target.value)} />
                        <label style={{ display: 'flex', gap: '0.25rem', alignItems: 'center' }}>
                          <input type="checkbox" checked={editPublic} onChange={(e) => setEditPublic(e.target.checked)} />
                          Public
                        </label>
                        <select multiple size={Math.min(agents.length, 6)} style={{ minWidth: '14rem' }}
                          value={editAgentIds}
                          onChange={(e) => setEditAgentIds(Array.from(e.target.selectedOptions, (o) => o.value))}>
                          {agents.map((a) => <option key={a.id} value={a.id}>{a.name}</option>)}
                        </select>
                        <select value={editGroupId} onChange={(e) => setEditGroupId(Number(e.target.value))}>
                          {globalAccount ? <option value={0}>No group</option> : null}
                          {groups.map((group) => <option key={group.group_id} value={group.group_id}>{group.name}</option>)}
                        </select>
                      </div>
                    </td>
                    <td>{new Date(v.created_at).toLocaleDateString()}</td>
                    <td>
                      <ActionButton busy={busyAction === `view:save:${v.view_id}`} label="Save" icon={<Check size={14} />}
                        onClick={() => void runAction(`view:save:${v.view_id}`, () => updateView(v.view_id, editName, editAgentIds, editPublic, editGroupId).then(() => {})).then(() => setEditId(null))} />
                      <ActionButton busy={false} label="Cancel" icon={<X size={14} />} onClick={() => setEditId(null)} />
                    </td>
                  </>
                ) : (
                  <>
                    <td><strong>{v.name}</strong></td>
                    <td>{v.owner_username}</td>
                    <td>{v.agent_ids.length} agent{v.agent_ids.length !== 1 ? 's' : ''}</td>
                    <td>{v.is_public ? 'Public' : 'Private'}</td>
                    <td>{groups.find((group) => group.group_id === v.group_id)?.name ?? 'No group'}</td>
                    <td>{new Date(v.created_at).toLocaleDateString()}</td>
                    <td>
                      <ActionButton busy={false} label="Open" icon={<Search size={14} />} onClick={() => onOpenView(v.view_id)} />
                      {canEdit(v) ? <ActionButton busy={false} label="Edit" icon={<SlidersHorizontal size={14} />} onClick={() => startEdit(v)} /> : null}
                      {canEdit(v) ? (
                        <ActionButton busy={busyAction === `view:delete:${v.view_id}`} danger label="Delete"
                          icon={<Trash2 size={14} />}
                          onClick={() => void runAction(`view:delete:${v.view_id}`, () => deleteView(v.view_id))} />
                      ) : null}
                    </td>
                  </>
                )}
              </tr>
            ))}
            {views.length === 0 ? (
              <tr><td colSpan={7} style={{ textAlign: 'center', padding: '1rem' }}>No views yet. Create one below.</td></tr>
            ) : null}
          </tbody>
        </table>
      </div>

      {operator ? (
        <div className="table-container" style={{ marginTop: '1rem' }}>
          <form
            onSubmit={(e) => {
              e.preventDefault();
              void runAction('view:create', () => createView(newName, newAgentIds, newPublic, newGroupId).then(() => {})).then(() => {
                setNewName('');
                setNewPublic(false);
                setNewAgentIds([]);
              });
            }}
            style={{ padding: '1rem', display: 'flex', gap: '0.75rem', flexWrap: 'wrap', alignItems: 'flex-start' }}
          >
            <input required value={newName} placeholder="New view name" onChange={(e) => setNewName(e.target.value)} style={{ minWidth: '12rem' }} />
            <label style={{ display: 'flex', gap: '0.25rem', alignItems: 'center' }}>
              <input type="checkbox" checked={newPublic} onChange={(e) => setNewPublic(e.target.checked)} />
              Public
            </label>
            <select multiple size={Math.min(agents.length, 6)} style={{ minWidth: '14rem' }}
              value={newAgentIds}
              onChange={(e) => setNewAgentIds(Array.from(e.target.selectedOptions, (o) => o.value))}>
              {agents.map((a) => <option key={a.id} value={a.id}>{a.name}</option>)}
            </select>
            <select value={newGroupId} onChange={(e) => setNewGroupId(Number(e.target.value))}>
              {globalAccount ? <option value={0}>No group</option> : null}
              {groups.map((group) => <option key={group.group_id} value={group.group_id}>{group.name}</option>)}
            </select>
            <button className="text-button" type="submit" disabled={busyAction === 'view:create' || !newName}>
              {busyAction === 'view:create' ? '...' : 'Create View'}
            </button>
          </form>
        </div>
      ) : null}
    </main>
  );
}

createRoot(document.getElementById('root')!).render(
  <React.StrictMode>
    <Dashboard />
  </React.StrictMode>,
);
