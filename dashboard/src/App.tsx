import React from 'react';
import { LogOut, RefreshCw } from 'lucide-react';
import { canManageAgent } from './agentActions';
import { logout } from './api';
import { AgentConfigModal } from './components/AgentConfigModal';
import { AgentIdentity } from './components/AgentIdentity';
import { AppFooter } from './components/AppFooter';
import { NavTab } from './components/NavTab';
import { SummaryCard } from './components/SummaryCard';
import { useActiveView } from './hooks/useActiveView';
import { useDashboardData } from './hooks/useDashboardData';
import { useRunAction } from './hooks/useRunAction';
import { useSession } from './hooks/useSession';
import { AgentsPage } from './pages/AgentsPage';
import { AlertsPage } from './pages/AlertsPage';
import { LoginPage } from './pages/LoginPage';
import { MaintenancePage } from './pages/MaintenancePage';
import { MonitoringPage } from './pages/MonitoringPage';
import { SettingsPage } from './pages/SettingsPage';
import { SilencesPage } from './pages/SilencesPage';
import { UsersPage } from './pages/UsersPage';
import { ViewsPage } from './pages/ViewsPage';
import { canOperate, isGlobal } from './permissions';
import { summaryCounts, toDisplayAlerts, type OverviewGroupFilter } from './status';
import { NAV_ITEMS, accountTypeLabel, colorLabel, type DashboardView } from './uiLogic';

/** App shell: session gate, top bar, summary cards and the page router. */
export function App() {
  const { session, setSession } = useSession();
  const [view, setView] = React.useState<DashboardView>('monitoring');
  const { activeViewId, setActiveViewId } = useActiveView();
  const [expanded, setExpanded] = React.useState<Set<string>>(new Set());
  const [configAgentId, setConfigAgentId] = React.useState<string | null>(null);
  const [overviewGroupFilter, setOverviewGroupFilter] = React.useState<OverviewGroupFilter>('all');

  const data = useDashboardData(session);
  const { agents, pending, groups, alerts, allAlerts, users, maintenanceWindows, silences, customViews, loadedAt, error, loading, refresh } = data;
  const { busyAction, runAction } = useRunAction(refresh, data.setError);

  if (!session) {
    return <LoginPage onLogin={setSession} />;
  }

  const counts = summaryCounts(agents);
  const displayAlerts = toDisplayAlerts(alerts, agents);
  const global = isGlobal(session.role);
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
            return <NavTab key={item.view} view={view} target={item.view} setView={setView} label={label} />;
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
              <button className="text-button banner-action" onClick={() => setActiveViewId(null)}>Clear</button>
            </div>
          ) : null}
          <MonitoringPage
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
        <AgentsPage
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
        <MaintenancePage agents={agents} busyAction={busyAction} global={global} operator={operator} runAction={runAction} windows={maintenanceWindows} />
      ) : null}
      {view === 'silences' ? (
        <SilencesPage agents={agents} busyAction={busyAction} global={global} operator={operator} runAction={runAction} silences={silences} />
      ) : null}
      {view === 'users' ? <UsersPage busyAction={busyAction} groups={groups} runAction={runAction} session={session} users={users} /> : null}
      {view === 'settings' ? <SettingsPage runAction={runAction} /> : null}
      <AppFooter />
    </>
  );
}
