import React from 'react';
import { loadDashboardData, type DashboardData } from '../api';
import { toDashboardAgents } from '../status';
import { usePolling } from './usePolling';

const POLL_INTERVAL_MS = 5000;

const SECTION_KEYS = ['pending', 'groups', 'alerts', 'allAlerts', 'users', 'maintenanceWindows', 'silences', 'views'] as const;

/**
 * A section that failed this time keeps its last good value, so a transient error
 * cannot turn a list of alerts into an empty (all-clear looking) one.
 */
export function keepFailedSections(next: DashboardData, previous: DashboardData | null): DashboardData {
  if (!previous) return next;
  const merged = { ...next };
  for (const key of SECTION_KEYS) {
    if (next.errors[key]) (merged[key] as unknown) = previous[key];
  }
  return merged;
}

/** Loads everything the dashboard shows and re-polls every few seconds. */
export function useDashboardData() {
  // usePolling applies the merge only to the accepted (latest) response, so an overtaken
  // older load can never become the "last good" snapshot.
  const { data, error, loading, loadedAt, refresh } = usePolling(loadDashboardData, { intervalMs: POLL_INTERVAL_MS, merge: keepFailedSections });

  const agents = React.useMemo(() => (data ? toDashboardAgents(data.agents, data.metrics, data.alerts) : []), [data]);
  const pending = React.useMemo(() => (data ? toDashboardAgents(data.pending, [], []) : []), [data]);

  return {
    agents,
    pending,
    groups: data?.groups ?? [],
    alerts: data?.alerts ?? [],
    allAlerts: data?.allAlerts ?? [],
    users: data?.users ?? [],
    maintenanceWindows: data?.maintenanceWindows ?? [],
    silences: data?.silences ?? [],
    customViews: data?.views ?? [],
    /** Per-section load failures; the section's list is its last good value (or empty). */
    sectionErrors: data?.errors ?? {},
    loadedAt,
    error,
    loading,
    refresh,
  };
}
