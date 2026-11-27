import React from 'react';
import { fetchViews, loadDashboardData } from '../api';
import type { AlertRecord, DashboardAgent, GroupRecord, MaintenanceWindowRecord, SessionInfo, SilenceRecord, UserRecord, ViewRecord } from '../models';
import { toDashboardAgents } from '../status';

const POLL_INTERVAL_MS = 5000;

/** Loads everything the dashboard shows and re-polls while a session is active. */
export function useDashboardData(session: SessionInfo | null) {
  const [agents, setAgents] = React.useState<DashboardAgent[]>([]);
  const [pending, setPending] = React.useState<DashboardAgent[]>([]);
  const [groups, setGroups] = React.useState<GroupRecord[]>([]);
  const [alerts, setAlerts] = React.useState<AlertRecord[]>([]);
  const [allAlerts, setAllAlerts] = React.useState<AlertRecord[]>([]);
  const [users, setUsers] = React.useState<UserRecord[]>([]);
  const [maintenanceWindows, setMaintenanceWindows] = React.useState<MaintenanceWindowRecord[]>([]);
  const [silences, setSilences] = React.useState<SilenceRecord[]>([]);
  const [customViews, setCustomViews] = React.useState<ViewRecord[]>([]);
  const [loadedAt, setLoadedAt] = React.useState<Date | null>(null);
  const [error, setError] = React.useState<string | null>(null);
  const [loading, setLoading] = React.useState(true);

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
    void refresh();
    const timer = window.setInterval(() => void refresh(), POLL_INTERVAL_MS);
    return () => window.clearInterval(timer);
  }, [refresh]);

  return { agents, pending, groups, alerts, allAlerts, users, maintenanceWindows, silences, customViews, loadedAt, error, setError, loading, refresh };
}
