import React from 'react';
import { LogOut, RefreshCw } from 'lucide-react';
import { canManageAgent } from './agentActions';
import { ActionErrorBanner } from './components/ActionError';
import { AgentConfigModal } from './components/AgentConfigModal';
import { AgentIdentity } from './components/AgentIdentity';
import { AppFooter } from './components/AppFooter';
import { LoadFailed } from './components/LoadFailed';
import { NavTab } from './components/NavTab';
import { SummaryCard } from './components/SummaryCard';
import { useActiveView } from './hooks/useActiveView';
import { useDashboardData } from './hooks/useDashboardData';
import { ActionFailureContext, useRunAction } from './hooks/useRunAction';
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
import type { SessionInfo } from './models';
import { canDeleteAgents, canEditWebhooks, canOperate, isGlobal } from './permissions';
import { summaryCounts, toDisplayAlerts, type OverviewGroupFilter } from './status';
import { NAV_ITEMS, accountTypeLabel, colorLabel, type DashboardView } from './uiLogic';

/** Session gate: the login page until signed in, then the dashboard. */
export function App() {
  const { session, expired, signingOut, signOutError, signIn, signOut, dismissSignOutError } = useSession();
  if (!session) {
    return <LoginPage onLogin={signIn} notice={expired ? 'Your session expired. Sign in again.' : null} />;
  }
  return (
    <Dashboard
      session={session}
      signingOut={signingOut}
      signOutError={signOutError}
      onDismissSignOutError={dismissSignOutError}
      onSignOut={() => void signOut()}
    />
  );
}

/** Everything behind the login: top bar, summary cards and the page router. Unmounting it stops all polling. */
function Dashboard({
  session,
  signingOut,
  signOutError,
  onDismissSignOutError,
  onSignOut,
}: {
  session: SessionInfo;
  signingOut: boolean;
  signOutError: string | null;
  onDismissSignOutError: () => void;
  onSignOut: () => void;
}) {
  const [view, setView] = React.useState<DashboardView>('monitoring');
  const { activeViewId, setActiveViewId } = useActiveView();
  const [expanded, setExpanded] = React.useState<Set<string>>(new Set());
  const [configAgentId, setConfigAgentId] = React.useState<string | null>(null);
  const [overviewGroupFilter, setOverviewGroupFilter] = React.useState<OverviewGroupFilter>('all');

  const { agents, pending, groups, alerts, allAlerts, users, maintenanceWindows, silences, customViews, sectionErrors, loadedAt, error, loading, refresh } = useDashboardData();
  const { isBusy, runAction, clearFailure, context: failureState } = useRunAction(refresh);

  // A bookmark (#view-N) to a view that was deleted, or that this user may not
  // open, must not leave the page filtered to nothing: the filter is ignored
  // and a notice says why. Only judged once the views loaded, so a slow or
  // failed load cannot discard a good bookmark.
  const activeView = customViews.find((candidate) => candidate.view_id === activeViewId) ?? null;
  const viewMissing = activeViewId != null && loadedAt !== null && !sectionErrors.views && activeView === null;

  const counts = summaryCounts(agents);
  const displayAlerts = toDisplayAlerts(alerts, agents);
  const global = isGlobal(session.role);
  const operator = canOperate(session.role);
  const configAgent = agents.find((agent) => agent.id === configAgentId) ?? null;
  // Until the first load succeeds there is nothing real to show, so the pages
  // are replaced by "loading" or "load failed". Settings loads for itself.
  const unloaded = loadedAt === null && view !== 'settings';
  const count = (n: number) => (loadedAt ? n : '—');
  const changeView = (next: DashboardView) => {
    clearFailure();
    setView(next);
  };
  const retry = () => void refresh();

  return (
    <ActionFailureContext.Provider value={failureState}>
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
            return <NavTab key={item.view} view={view} target={item.view} setView={changeView} label={label} />;
          })}
        </nav>
        <div className="topbar-meta">
          <span>
            <strong>{loadedAt ? loadedAt.toUTCString().replace('GMT', 'UTC') : 'Not synced'}</strong>
          </span>
          <span>
            {session.username} <strong>{accountTypeLabel(session.role)}</strong>
          </span>
          <button className="icon-button" onClick={retry} aria-label="Refresh dashboard">
            <RefreshCw size={14} />
          </button>
          <button className="icon-button" disabled={signingOut} onClick={onSignOut} aria-label="Log out">
            <LogOut size={14} />
          </button>
        </div>
      </header>

      <section className="summary">
        <SummaryCard label="Healthy" value={count(counts.green)} color="green" />
        <SummaryCard label="Warning" value={count(counts.yellow)} color="yellow" />
        <SummaryCard label="Degraded" value={count(counts.amber)} color="amber" />
        <SummaryCard label="Critical" value={count(counts.red)} color="red" />
        <SummaryCard label="Maintenance" value={count(counts.blue)} color="blue" />
        <SummaryCard label="Offline" value={count(counts.offline)} color="grey" />
      </section>

      {/* A failed refresh with older data on screen is flagged; with no data yet it replaces the page below. */}
      {error && loadedAt ? <LoadFailed what="dashboard data" message={error} stale onRetry={retry} /> : null}
      <ActionErrorBanner onDismiss={() => clearFailure()} />
      {signOutError ? (
        <div className="banner error" role="alert">
          {signOutError}
          <button className="text-button banner-action" disabled={signingOut} onClick={onSignOut} type="button">Try again</button>
          <button className="text-button banner-action" onClick={onDismissSignOutError} type="button">Dismiss</button>
        </div>
      ) : null}

      {unloaded && error ? <LoadFailed what="dashboard data" message={error} onRetry={retry} /> : null}
      {unloaded && !error && loading ? <div className="banner">Loading dashboard data...</div> : null}

      {!unloaded && view === 'monitoring' ? (
        <>
          {sectionErrors.alerts ? <LoadFailed what="alerts" message={sectionErrors.alerts} onRetry={retry} /> : null}
          {sectionErrors.groups ? <LoadFailed what="groups" message={sectionErrors.groups} onRetry={retry} /> : null}
          {alerts.length > 0 ? (
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
          {viewMissing ? (
            <div className="banner" role="status">
              View {activeViewId} was not found: it may have been deleted or you may not have access to it.
              <button className="text-button banner-action" onClick={() => setActiveViewId(null)}>Dismiss</button>
            </div>
          ) : null}
          {activeViewId != null && !viewMissing ? (
            <div className="banner">
              Filtered to view: <strong>{activeView?.name ?? 'Loading…'}</strong>
              <button className="text-button banner-action" onClick={() => setActiveViewId(null)}>Clear</button>
            </div>
          ) : null}
          <MonitoringPage
            agents={activeView ? agents.filter((a) => activeView.agent_ids.includes(a.id)) : viewMissing ? agents : activeViewId != null ? [] : agents}
            expanded={expanded}
            groupFilter={overviewGroupFilter}
            groups={groups}
            global={global}
            setExpanded={setExpanded}
            setGroupFilter={setOverviewGroupFilter}
          />
        </>
      ) : null}
      {!unloaded && view === 'views' ? (
        <>
          {sectionErrors.views ? <LoadFailed what="views" message={sectionErrors.views} onRetry={retry} /> : null}
          {sectionErrors.groups ? <LoadFailed what="groups" message={sectionErrors.groups} onRetry={retry} /> : null}
          <ViewsPage
            agents={agents}
            isBusy={isBusy}
            loadFailed={Boolean(sectionErrors.views)}
            operator={operator}
            runAction={runAction}
            session={session}
            views={customViews}
            onOpenView={(viewId) => { setActiveViewId(viewId); setView('monitoring'); }}
            groups={groups}
          />
        </>
      ) : null}
      {!unloaded && view === 'agents' ? (
        <>
          {sectionErrors.pending ? <LoadFailed what="pending enrollments" message={sectionErrors.pending} onRetry={retry} /> : null}
          {sectionErrors.groups ? <LoadFailed what="groups" message={sectionErrors.groups} onRetry={retry} /> : null}
          <AgentsPage
            agents={agents}
            isBusy={isBusy}
            groups={groups}
            onConfigure={setConfigAgentId}
            canDelete={canDeleteAgents(session.role)}
            operator={operator}
            pending={global && operator ? pending : []}
            pendingLoadFailed={Boolean(sectionErrors.pending)}
            showPending={global && operator}
            runAction={runAction}
          />
        </>
      ) : null}
      {configAgent ? (
        <AgentConfigModal
          agent={configAgent}
          busy={isBusy(`${configAgent.id}:collector_config`)}
          onClose={() => setConfigAgentId(null)}
          operator={operator && canManageAgent(configAgent)}
          global={global}
          groups={groups}
          runAction={runAction}
        />
      ) : null}
      {!unloaded && view === 'alerts' ? (
        <>
          {sectionErrors.allAlerts ? <LoadFailed what="alerts" message={sectionErrors.allAlerts} onRetry={retry} /> : null}
          <AlertsPage agents={agents} alerts={toDisplayAlerts(allAlerts, agents)} isBusy={isBusy} operator={operator} runAction={runAction} />
        </>
      ) : null}
      {!unloaded && view === 'maintenance' ? (
        <>
          {sectionErrors.maintenanceWindows ? <LoadFailed what="maintenance windows" message={sectionErrors.maintenanceWindows} onRetry={retry} /> : null}
          <MaintenancePage agents={agents} isBusy={isBusy} global={global} loadFailed={Boolean(sectionErrors.maintenanceWindows)} operator={operator} runAction={runAction} windows={maintenanceWindows} />
        </>
      ) : null}
      {!unloaded && view === 'silences' ? (
        <>
          {sectionErrors.silences ? <LoadFailed what="silence rules" message={sectionErrors.silences} onRetry={retry} /> : null}
          <SilencesPage agents={agents} isBusy={isBusy} global={global} loadFailed={Boolean(sectionErrors.silences)} operator={operator} runAction={runAction} silences={silences} />
        </>
      ) : null}
      {!unloaded && view === 'users' ? (
        <>
          {sectionErrors.users ? <LoadFailed what="users" message={sectionErrors.users} onRetry={retry} /> : null}
          {sectionErrors.groups ? <LoadFailed what="groups" message={sectionErrors.groups} onRetry={retry} /> : null}
          <UsersPage isBusy={isBusy} groups={groups} runAction={runAction} session={session} users={users} />
        </>
      ) : null}
      {view === 'settings' ? <SettingsPage canEditWebhooks={canEditWebhooks(session.role)} runAction={runAction} /> : null}
      <AppFooter />
    </ActionFailureContext.Provider>
  );
}
