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

async function fetchJson<T>(path: string): Promise<T> {
  const response = await fetch(`${API_BASE}${path}`, { credentials: 'include' });
  if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
  return response.json() as Promise<T>;
}

async function mutateJson<T>(path: string, init: RequestInit): Promise<T> {
  const response = await fetch(`${API_BASE}${path}`, { ...init, credentials: 'include' });
  if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
  return response.json() as Promise<T>;
}

function jsonPost<T>(path: string, body: unknown): Promise<T> {
  return mutateJson<T>(path, {
    body: JSON.stringify(body),
    headers: { 'Content-Type': 'application/json' },
    method: 'POST',
  });
}

export function fetchSession(): Promise<SessionInfo> {
  return fetchJson<SessionInfo>('/api/session');
}

export function login(username: string, password: string): Promise<SessionInfo> {
  return jsonPost<SessionInfo>('/api/login', { username, password });
}

export async function logout(): Promise<void> {
  await mutateJson<{ ok: boolean }>('/api/logout', { method: 'POST' });
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

export async function loadDashboardData(): Promise<{
  agents: AgentRecord[];
  pending: AgentRecord[];
  metrics: MetricsSnapshot[];
  groups: GroupRecord[];
  alerts: AlertRecord[];
  allAlerts: AlertRecord[];
  users: UserRecord[];
  maintenanceWindows: MaintenanceWindowRecord[];
  silences: SilenceRecord[];
}> {
  const [agents, pending, metrics, groups, alerts, allAlerts, users, maintenanceWindows, silences] =
    await Promise.all([
      fetchAgents(),
      fetchPendingEnrollments().catch(() => []),
      fetchLatestMetrics(),
      fetchGroups().catch(() => []),
      fetchUnacknowledgedAlerts().catch(() => []),
      fetchAlerts().catch(() => []),
      fetchUsers().catch(() => []),
      fetchMaintenanceWindows().catch(() => []),
      fetchSilences().catch(() => []),
    ]);
  return { agents, pending, metrics, groups, alerts, allAlerts, users, maintenanceWindows, silences };
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
