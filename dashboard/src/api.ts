import type {
  AgentCollectorConfigUpdate,
  AgentRecord,
  AlertRecord,
  GroupRecord,
  LogMatchRecord,
  MaintenanceWindowRecord,
  MetricsSnapshot,
  RunbookRecord,
  ServerSettings,
  SessionInfo,
  SilenceRecord,
  StatusHistoryRow,
  UserRecord,
  ViewRecord,
} from './models';

const API_BASE = import.meta.env.VITE_API_BASE_URL ?? '';

/** A failed API call. Keeps the HTTP status and the server's own `{"error": "..."}` message. */
export class ApiError extends Error {
  readonly status: number;

  constructor(status: number, message: string) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
  }
}

/** Message to show for any caught value: the server's text for an ApiError, otherwise the fallback. */
export function errorMessage(err: unknown, fallback: string): string {
  return err instanceof Error && err.message ? err.message : fallback;
}

// Session expiry is handled in one place: every request that comes back 401
// (except the sign-in probes, where 401 is the normal answer) tells the listeners,
// and useSession returns the UI to the login screen.
const unauthorizedListeners = new Set<() => void>();

/** Registers a callback for "the session is no longer valid"; returns the unsubscribe function. */
export function onUnauthorized(listener: () => void): () => void {
  unauthorizedListeners.add(listener);
  return () => unauthorizedListeners.delete(listener);
}

async function readError(response: Response): Promise<ApiError> {
  let message = `${response.status} ${response.statusText}`;
  try {
    const body: unknown = await response.json();
    if (body && typeof body === 'object' && 'error' in body && typeof body.error === 'string' && body.error) {
      message = body.error;
    }
  } catch {
    // No JSON body (proxy error page, empty response): the status line is all there is.
  }
  return new ApiError(response.status, message);
}

async function request<T>(path: string, init: RequestInit, signedOutIsNormal = false): Promise<T> {
  const response = await fetch(`${API_BASE}${path}`, { ...init, credentials: 'include' });
  if (!response.ok) {
    if (response.status === 401 && !signedOutIsNormal) unauthorizedListeners.forEach((listener) => listener());
    throw await readError(response);
  }
  return response.json() as Promise<T>;
}

function fetchJson<T>(path: string, signedOutIsNormal = false): Promise<T> {
  return request<T>(path, {}, signedOutIsNormal);
}

function mutateJson<T>(path: string, init: RequestInit, signedOutIsNormal = false): Promise<T> {
  return request<T>(path, init, signedOutIsNormal);
}

function jsonPost<T>(path: string, body: unknown, signedOutIsNormal = false): Promise<T> {
  return mutateJson<T>(
    path,
    {
      body: JSON.stringify(body),
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    },
    signedOutIsNormal,
  );
}

export function fetchSession(): Promise<SessionInfo> {
  return fetchJson<SessionInfo>('/api/session', true);
}

export function login(username: string, password: string): Promise<SessionInfo> {
  return jsonPost<SessionInfo>('/api/login', { username, password }, true);
}

export async function logout(): Promise<void> {
  await mutateJson<{ ok: boolean }>('/api/logout', { method: 'POST' }, true);
}

export async function fetchAgents(): Promise<AgentRecord[]> {
  return fetchJson<AgentRecord[]>('/api/agents');
}

export async function fetchPendingEnrollments(): Promise<AgentRecord[]> {
  return fetchJson<AgentRecord[]>('/api/pending-enrollments');
}

export async function fetchLatestMetrics(): Promise<MetricsSnapshot[]> {
  return fetchJson<MetricsSnapshot[]>('/api/metrics');
}

export async function fetchGroups(): Promise<GroupRecord[]> {
  return fetchJson<GroupRecord[]>('/api/groups');
}

export async function fetchUsers(): Promise<UserRecord[]> {
  return fetchJson<UserRecord[]>('/api/users');
}

export async function createGroup(name: string): Promise<void> {
  await jsonPost<{ group_id: number }>('/api/groups', { name });
}

export async function createUser(username: string, password: string, role: UserRecord['role'], groupIds: number[]): Promise<void> {
  await jsonPost<{ user_id: number }>('/api/users', { username, password, role, group_ids: groupIds });
}

export async function fetchAlerts(includeArchived = false): Promise<AlertRecord[]> {
  return fetchJson<AlertRecord[]>(includeArchived ? '/api/alerts?include_archived=1' : '/api/alerts');
}

export async function fetchUnacknowledgedAlerts(): Promise<AlertRecord[]> {
  return fetchJson<AlertRecord[]>('/api/alerts/unacknowledged');
}

/** Sections of the dashboard that load independently of the core agent/metrics data. */
export type DashboardSection =
  | 'pending'
  | 'groups'
  | 'alerts'
  | 'allAlerts'
  | 'users'
  | 'maintenanceWindows'
  | 'silences'
  | 'views';

export interface DashboardData {
  agents: AgentRecord[];
  pending: AgentRecord[];
  metrics: MetricsSnapshot[];
  groups: GroupRecord[];
  alerts: AlertRecord[];
  allAlerts: AlertRecord[];
  users: UserRecord[];
  maintenanceWindows: MaintenanceWindowRecord[];
  silences: SilenceRecord[];
  views: ViewRecord[];
  /** Sections that failed to load, with the reason. A failed section is empty here; it is not "no data". */
  errors: Partial<Record<DashboardSection, string>>;
}

/**
 * Loads everything the dashboard shows. Agents and metrics are required, so a
 * failure there rejects. The other sections load independently: a 403 means the
 * role may not see that section (shown as empty), any other failure is recorded
 * in `errors` so the UI can say the section could not be loaded.
 */
export async function loadDashboardData(): Promise<DashboardData> {
  const errors: DashboardData['errors'] = {};
  const section = <T>(name: DashboardSection, load: Promise<T[]>): Promise<T[]> =>
    load.catch((err: unknown) => {
      if (!(err instanceof ApiError && err.status === 403)) errors[name] = errorMessage(err, 'Failed to load');
      return [];
    });
  const [agents, metrics, pending, groups, alerts, allAlerts, users, maintenanceWindows, silences, views] =
    await Promise.all([
      fetchAgents(),
      fetchLatestMetrics(),
      section('pending', fetchPendingEnrollments()),
      section('groups', fetchGroups()),
      section('alerts', fetchUnacknowledgedAlerts()),
      section('allAlerts', fetchAlerts()),
      section('users', fetchUsers()),
      section('maintenanceWindows', fetchMaintenanceWindows()),
      section('silences', fetchSilences()),
      section('views', fetchViews()),
    ]);
  return { agents, pending, metrics, groups, alerts, allAlerts, users, maintenanceWindows, silences, views, errors };
}

export async function approveAgent(agentId: string, groupIds: number[] = []): Promise<void> {
  await jsonPost<{ ok: boolean }>(`/api/agents/${encodeURIComponent(agentId)}/approve`, { group_ids: groupIds });
}

export async function rejectAgent(agentId: string): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/agents/${encodeURIComponent(agentId)}/reject`, { method: 'POST' });
}

export async function deleteAgent(agentId: string): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/agents/${encodeURIComponent(agentId)}`, { method: 'DELETE' });
}

export async function setAgentCollectorConfig(agentId: string, update: AgentCollectorConfigUpdate): Promise<void> {
  await jsonPost<{ ok: boolean }>(`/api/agents/${encodeURIComponent(agentId)}/collector_config`, update);
}

export async function setMaintenance(agentId: string, reason: string, untilMs: number): Promise<void> {
  await jsonPost<{ ok: boolean }>(`/api/agents/${encodeURIComponent(agentId)}/maintenance`, {
    reason,
    until_ms: untilMs,
  });
}

export async function pauseAgent(agentId: string): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/agents/${encodeURIComponent(agentId)}/pause`, { method: 'POST' });
}

export async function resumeAgent(agentId: string): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/agents/${encodeURIComponent(agentId)}/resume`, { method: 'POST' });
}

export async function restartAgentCollectors(agentId: string): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/agents/${encodeURIComponent(agentId)}/restart_collectors`, { method: 'POST' });
}

export async function requestAgentStatus(agentId: string): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/agents/${encodeURIComponent(agentId)}/get_status`, { method: 'POST' });
}

export async function acknowledgeAlert(alertId: number, note = ''): Promise<void> {
  await jsonPost<{ ok: boolean }>(`/api/alerts/${alertId}/ack`, { note });
}

export async function bulkAcknowledgeAlerts(alertIds: number[], note = ''): Promise<void> {
  await jsonPost<{ ok: boolean }>('/api/alerts/bulk-ack', { alert_ids: alertIds, note });
}

export async function archiveAlert(alertId: number): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/alerts/${alertId}`, { method: 'DELETE' });
}

export async function bulkArchiveAlerts(alertIds: number[]): Promise<void> {
  await jsonPost<{ ok: boolean }>('/api/alerts/bulk-archive', { alert_ids: alertIds });
}

export async function fetchMetricHistory(agentId: string, limit = 20): Promise<MetricsSnapshot[]> {
  return fetchJson<MetricsSnapshot[]>(`/api/metrics/${encodeURIComponent(agentId)}?limit=${limit}`);
}

export async function fetchMaintenanceWindows(): Promise<MaintenanceWindowRecord[]> {
  return fetchJson<MaintenanceWindowRecord[]>('/api/maintenance-windows');
}

export async function createMaintenanceWindow(
  agentId: string,
  startMs: number,
  endMs: number,
  reason: string,
): Promise<void> {
  await jsonPost<{ window_id: number }>('/api/maintenance-windows', {
    agent_id: agentId,
    start_ms: startMs,
    end_ms: endMs,
    reason,
  });
}

export async function deleteMaintenanceWindow(windowId: number): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/maintenance-windows/${windowId}`, { method: 'DELETE' });
}

export async function setAgentDescription(agentId: string, description: string): Promise<void> {
  await jsonPost<{ ok: boolean }>(`/api/agents/${encodeURIComponent(agentId)}/description`, { description });
}

export async function fetchSettings(): Promise<ServerSettings> {
  return fetchJson<ServerSettings>('/api/settings');
}

export async function updateSettings(settings: Partial<ServerSettings>): Promise<void> {
  await mutateJson<{ ok: boolean }>('/api/settings', {
    body: JSON.stringify(settings),
    headers: { 'Content-Type': 'application/json' },
    method: 'PUT',
  });
}

export async function sendReport(): Promise<{ sent: boolean; report: unknown }> {
  return mutateJson<{ sent: boolean; report: unknown }>('/api/reports/send', { method: 'POST' });
}

export async function disableUser(userId: number): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/users/${userId}/disable`, { method: 'PUT' });
}

export async function enableUser(userId: number): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/users/${userId}/enable`, { method: 'PUT' });
}

export async function deleteUser(userId: number): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/users/${userId}`, { method: 'DELETE' });
}

/** Changes a user's role and/or group membership; omitted fields stay as they are. */
export async function updateUserAccess(userId: number, changes: { role?: UserRecord['role']; group_ids?: number[] }): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/users/${userId}`, {
    body: JSON.stringify(changes),
    headers: { 'Content-Type': 'application/json' },
    method: 'PUT',
  });
}

export async function changeUserPassword(userId: number, password: string): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/users/${userId}/password`, {
    body: JSON.stringify({ password }),
    headers: { 'Content-Type': 'application/json' },
    method: 'PUT',
  });
}

export async function fetchSilences(): Promise<SilenceRecord[]> {
  return fetchJson<SilenceRecord[]>('/api/silences');
}

export async function createSilence(
  agentId: string,
  indicator: string,
  reason: string,
  untilMs: number,
): Promise<void> {
  await jsonPost<{ silence_id: number }>('/api/silences', {
    agent_id: agentId,
    indicator,
    reason,
    until_ms: untilMs,
  });
}

export async function deleteSilence(silenceId: number): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/silences/${silenceId}`, { method: 'DELETE' });
}

export async function fetchAgentHistory(agentId: string, limit = 100): Promise<StatusHistoryRow[]> {
  return fetchJson<StatusHistoryRow[]>(
    `/api/agents/${encodeURIComponent(agentId)}/history?limit=${limit}`,
  );
}

export async function fetchLogMatches(agentId: string, limit = 200): Promise<LogMatchRecord[]> {
  return fetchJson<LogMatchRecord[]>(
    `/api/agents/${encodeURIComponent(agentId)}/log-matches?limit=${limit}`,
  );
}

export async function fetchViews(): Promise<ViewRecord[]> {
  return fetchJson<ViewRecord[]>('/api/views');
}

export async function createView(name: string, agentIds: string[], isPublic: boolean, groupId: number): Promise<ViewRecord> {
  return jsonPost<ViewRecord>('/api/views', { name, agent_ids: agentIds, is_public: isPublic, group_id: groupId });
}

export async function updateView(viewId: number, name: string, agentIds: string[], isPublic: boolean, groupId: number): Promise<ViewRecord> {
  return mutateJson<ViewRecord>(`/api/views/${viewId}`, {
    method: 'PUT',
    body: JSON.stringify({ name, agent_ids: agentIds, is_public: isPublic, group_id: groupId }),
    headers: { 'Content-Type': 'application/json' },
  });
}

export async function deleteView(viewId: number): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/views/${viewId}`, { method: 'DELETE' });
}

export async function fetchRunbooks(): Promise<RunbookRecord[]> {
  return fetchJson<RunbookRecord[]>('/api/runbooks');
}

export async function createRunbook(indicator: string, status: string, url: string, notes: string): Promise<RunbookRecord> {
  return jsonPost<RunbookRecord>('/api/runbooks', { indicator, status, url, notes });
}

export async function deleteRunbook(runbookId: number): Promise<void> {
  await mutateJson<{ ok: boolean }>(`/api/runbooks/${runbookId}`, { method: 'DELETE' });
}
