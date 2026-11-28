import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  acknowledgeAlert,
  archiveAlert,
  approveAgent,
  bulkAcknowledgeAlerts,
  bulkArchiveAlerts,
  changeUserPassword,
  createView,
  createGroup,
  createMaintenanceWindow,
  createSilence,
  createUser,
  deleteAgent,
  deleteMaintenanceWindow,
  deleteSilence,
  deleteUser,
  disableUser,
  enableUser,
  fetchAgentHistory,
  fetchAgents,
  fetchAlerts,
  fetchGroups,
  fetchLatestMetrics,
  fetchMaintenanceWindows,
  fetchMetricHistory,
  fetchPendingEnrollments,
  fetchSession,
  fetchSettings,
  fetchSilences,
  fetchUnacknowledgedAlerts,
  fetchUsers,
  loadDashboardData,
  login,
  logout,
  pauseAgent,
  rejectAgent,
  requestAgentStatus,
  restartAgentCollectors,
  resumeAgent,
  setAgentDescription,
  setAgentCollectorConfig,
  setMaintenance,
  updateSettings,
  updateView,
} from './api';

describe('GIVEN agent management API actions', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('WHEN enrollment actions are requested THEN the expected backend endpoints are called', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await approveAgent('agent-1', [1]);
    await rejectAgent('agent-1');
    await deleteAgent('agent-1');

    expect(fetchMock).toHaveBeenNthCalledWith(1, '/api/agents/agent-1/approve', {
      body: JSON.stringify({ group_ids: [1] }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
    expect(fetchMock).toHaveBeenNthCalledWith(2, '/api/agents/agent-1/reject', {
      credentials: 'include',
      method: 'POST',
    });
    expect(fetchMock).toHaveBeenNthCalledWith(3, '/api/agents/agent-1', {
      credentials: 'include',
      method: 'DELETE',
    });
  });

  it('WHEN settings are changed THEN command payloads are sent to the agent endpoints', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await setAgentCollectorConfig('agent-2', {
      collection_interval: 30,
      heartbeat_interval: 5,
      process_limit: 12,
      collector_config: {
        cpu: { warning_percent: 80, degraded_percent: 90, critical_percent: 95 },
        memory: { warning_percent: 80, degraded_percent: 90, critical_percent: 95 },
        cpu_readings: 2,
        memory_readings: 2,
        disk_readings: 3,
        network_readings: 4,
        process_readings: 3,
        disks: [{ mount_point: '/', device: '/dev/sda1', enabled: true, thresholds: { warning_percent: 80, degraded_percent: 90, critical_percent: 95 } }],
        networks: [{ interface_name: 'eth0', enabled: true, thresholds: { warning_mbps: 100, degraded_mbps: 200, critical_mbps: 300 } }],
        processes: [{ name: 'TheWatcherAgent.exe', expected_count: 1, enabled: true }],
      },
    });
    await pauseAgent('agent-2');
    await resumeAgent('agent-2');
    await restartAgentCollectors('agent-2');
    await requestAgentStatus('agent-2');

    expect(fetchMock).toHaveBeenNthCalledWith(1, '/api/agents/agent-2/collector_config', {
      body: JSON.stringify({
        collection_interval: 30,
        heartbeat_interval: 5,
        process_limit: 12,
        collector_config: {
          cpu: { warning_percent: 80, degraded_percent: 90, critical_percent: 95 },
          memory: { warning_percent: 80, degraded_percent: 90, critical_percent: 95 },
          cpu_readings: 2,
          memory_readings: 2,
          disk_readings: 3,
          network_readings: 4,
          process_readings: 3,
          disks: [{ mount_point: '/', device: '/dev/sda1', enabled: true, thresholds: { warning_percent: 80, degraded_percent: 90, critical_percent: 95 } }],
          networks: [{ interface_name: 'eth0', enabled: true, thresholds: { warning_mbps: 100, degraded_mbps: 200, critical_mbps: 300 } }],
          processes: [{ name: 'TheWatcherAgent.exe', expected_count: 1, enabled: true }],
        },
      }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
    expect(fetchMock).toHaveBeenNthCalledWith(2, '/api/agents/agent-2/pause', { credentials: 'include', method: 'POST' });
    expect(fetchMock).toHaveBeenNthCalledWith(3, '/api/agents/agent-2/resume', { credentials: 'include', method: 'POST' });
    expect(fetchMock).toHaveBeenNthCalledWith(4, '/api/agents/agent-2/restart_collectors', {
      credentials: 'include',
      method: 'POST',
    });
    expect(fetchMock).toHaveBeenNthCalledWith(5, '/api/agents/agent-2/get_status', {
      credentials: 'include',
      method: 'POST',
    });
  });

  it('WHEN alert actions are performed THEN the correct endpoints and payloads are used', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await acknowledgeAlert(42, 'looks like a spike');
    await acknowledgeAlert(43);
    await archiveAlert(44);
    await bulkAcknowledgeAlerts([10, 11, 12], 'batch ack note');
    await bulkArchiveAlerts([20, 21]);

    expect(fetchMock).toHaveBeenNthCalledWith(1, '/api/alerts/42/ack', {
      body: JSON.stringify({ note: 'looks like a spike' }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
    expect(fetchMock).toHaveBeenNthCalledWith(2, '/api/alerts/43/ack', {
      body: JSON.stringify({ note: '' }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
    expect(fetchMock).toHaveBeenNthCalledWith(3, '/api/alerts/44', {
      credentials: 'include',
      method: 'DELETE',
    });
    expect(fetchMock).toHaveBeenNthCalledWith(4, '/api/alerts/bulk-ack', {
      body: JSON.stringify({ alert_ids: [10, 11, 12], note: 'batch ack note' }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
    expect(fetchMock).toHaveBeenNthCalledWith(5, '/api/alerts/bulk-archive', {
      body: JSON.stringify({ alert_ids: [20, 21] }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
  });

  it('WHEN alerts are fetched THEN include_archived param controls whether deleted alerts are included', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => [] });
    vi.stubGlobal('fetch', fetchMock);

    await fetchAlerts();
    await fetchAlerts(false);
    await fetchAlerts(true);

    expect(fetchMock).toHaveBeenNthCalledWith(1, '/api/alerts', { credentials: 'include' });
    expect(fetchMock).toHaveBeenNthCalledWith(2, '/api/alerts', { credentials: 'include' });
    expect(fetchMock).toHaveBeenNthCalledWith(3, '/api/alerts?include_archived=1', { credentials: 'include' });
  });

  it('WHEN users and groups are created THEN admin payloads are sent to the backend', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ user_id: 7, group_id: 3 }) });
    vi.stubGlobal('fetch', fetchMock);

    await createGroup('Production');
    await createUser('operator1', 'secret', 'group_operator', [3]);

    expect(fetchMock).toHaveBeenNthCalledWith(1, '/api/groups', {
      body: JSON.stringify({ name: 'Production' }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
    expect(fetchMock).toHaveBeenNthCalledWith(2, '/api/users', {
      body: JSON.stringify({ username: 'operator1', password: 'secret', role: 'group_operator', group_ids: [3] }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
  });

  it('WHEN metric history is fetched THEN the correct agent endpoint is called with a limit', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => [] });
    vi.stubGlobal('fetch', fetchMock);

    await fetchMetricHistory('agent-1', 20);

    expect(fetchMock).toHaveBeenCalledWith('/api/metrics/agent-1?limit=20', { credentials: 'include' });
  });

  it('WHEN maintenance windows are fetched THEN the windows endpoint is called', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => [] });
    vi.stubGlobal('fetch', fetchMock);

    await fetchMaintenanceWindows();

    expect(fetchMock).toHaveBeenCalledWith('/api/maintenance-windows', { credentials: 'include' });
  });

  it('WHEN a maintenance window is created THEN the correct payload is sent', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ window_id: 1 }) });
    vi.stubGlobal('fetch', fetchMock);

    await createMaintenanceWindow('agent-1', 1000, 4600000, 'weekly patching');

    expect(fetchMock).toHaveBeenCalledWith('/api/maintenance-windows', {
      body: JSON.stringify({ agent_id: 'agent-1', start_ms: 1000, end_ms: 4600000, reason: 'weekly patching' }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
  });

  it('WHEN a maintenance window is deleted THEN the delete endpoint is called', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await deleteMaintenanceWindow(42);

    expect(fetchMock).toHaveBeenCalledWith('/api/maintenance-windows/42', { credentials: 'include', method: 'DELETE' });
  });

  it('WHEN agent description is set THEN the description endpoint is called with the new value', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await setAgentDescription('agent-1', 'Production web server');

    expect(fetchMock).toHaveBeenCalledWith('/api/agents/agent-1/description', {
      body: JSON.stringify({ description: 'Production web server' }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
  });

  it('WHEN views are created and updated THEN the selected group is included', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ view_id: 1 }) });
    vi.stubGlobal('fetch', fetchMock);

    await createView('Production', ['agent-1'], false, 7);
    await updateView(1, 'Production Linux', ['agent-2'], false, 8);

    expect(fetchMock).toHaveBeenNthCalledWith(1, '/api/views', {
      body: JSON.stringify({ name: 'Production', agent_ids: ['agent-1'], is_public: false, group_id: 7 }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
    expect(fetchMock).toHaveBeenNthCalledWith(2, '/api/views/1', {
      body: JSON.stringify({ name: 'Production Linux', agent_ids: ['agent-2'], is_public: false, group_id: 8 }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'PUT',
    });
  });

  it('WHEN settings are fetched THEN the settings endpoint is called', async () => {
    const fetchMock = vi.fn().mockResolvedValue({
      ok: true,
      json: async () => ({ webhook_url: 'https://hooks.example.com', offline_after_seconds: 120, escalation_timeout_seconds: 300 }),
    });
    vi.stubGlobal('fetch', fetchMock);

    const settings = await fetchSettings();

    expect(fetchMock).toHaveBeenCalledWith('/api/settings', { credentials: 'include' });
    expect(settings.webhook_url).toBe('https://hooks.example.com');
    expect(settings.offline_after_seconds).toBe(120);
  });

  it('WHEN settings are updated THEN the PUT endpoint is called with the new values', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await updateSettings({ webhook_url: 'https://hooks.example.com/new', offline_after_seconds: 180 });

    expect(fetchMock).toHaveBeenCalledWith('/api/settings', {
      body: JSON.stringify({ webhook_url: 'https://hooks.example.com/new', offline_after_seconds: 180 }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'PUT',
    });
  });

  it('WHEN user management actions are performed THEN the correct endpoints are called', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await disableUser(5);
    await enableUser(5);
    await deleteUser(5);
    await changeUserPassword(5, 'newpass123');

    expect(fetchMock).toHaveBeenNthCalledWith(1, '/api/users/5/disable', { credentials: 'include', method: 'PUT' });
    expect(fetchMock).toHaveBeenNthCalledWith(2, '/api/users/5/enable', { credentials: 'include', method: 'PUT' });
    expect(fetchMock).toHaveBeenNthCalledWith(3, '/api/users/5', { credentials: 'include', method: 'DELETE' });
    expect(fetchMock).toHaveBeenNthCalledWith(4, '/api/users/5/password', {
      body: JSON.stringify({ password: 'newpass123' }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'PUT',
    });
  });

  it('WHEN a user changes their own password THEN the current password is sent with the new one', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await changeUserPassword(5, 'newpass123', 'oldpass456');

    expect(fetchMock).toHaveBeenCalledWith('/api/users/5/password', {
      body: JSON.stringify({ password: 'newpass123', current_password: 'oldpass456' }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'PUT',
    });
  });

  it('WHEN an admin resets another user THEN no current_password key is sent', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await changeUserPassword(6, 'newpass123');

    const body = JSON.parse(fetchMock.mock.calls[0][1].body as string) as Record<string, unknown>;
    expect(body).toEqual({ password: 'newpass123' });
    expect(Object.keys(body)).not.toContain('current_password');
  });

  it('WHEN the server refuses a wrong current password THEN the server message reaches the caller', async () => {
    const fetchMock = vi.fn().mockResolvedValue({
      ok: false,
      status: 403,
      statusText: 'Forbidden',
      json: async () => ({ error: 'current password is incorrect' }),
    });
    vi.stubGlobal('fetch', fetchMock);

    await expect(changeUserPassword(5, 'newpass123', 'wrong')).rejects.toThrow('current password is incorrect');
  });

  it('WHEN the error body never arrives THEN the request still rejects with the status line after the wait', async () => {
    vi.useFakeTimers();
    try {
      const fetchMock = vi.fn().mockResolvedValue({
        ok: false,
        status: 403,
        statusText: 'Forbidden',
        json: () => new Promise(() => undefined),
      });
      vi.stubGlobal('fetch', fetchMock);

      const outcome = changeUserPassword(5, 'newpass123', 'wrong').then(
        () => 'resolved',
        (err: Error) => err.message,
      );
      await vi.advanceTimersByTimeAsync(2500);

      await expect(outcome).resolves.toBe('403 Forbidden');
    } finally {
      vi.useRealTimers();
    }
  });

  it('WHEN setMaintenance is called THEN the correct payload is sent to the agent endpoint', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await setMaintenance('agent-1', 'patch window', 1234567890);

    expect(fetchMock).toHaveBeenCalledWith('/api/agents/agent-1/maintenance', {
      body: JSON.stringify({ reason: 'patch window', until_ms: 1234567890 }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
  });
});

describe('GIVEN read-only fetch endpoints', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('WHEN fetchSession is called THEN GET /api/session is called', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ username: 'admin', role: 'global_admin' }) });
    vi.stubGlobal('fetch', fetchMock);
    const session = await fetchSession();
    expect(fetchMock).toHaveBeenCalledWith('/api/session', { credentials: 'include' });
    expect(session.username).toBe('admin');
  });

  it('WHEN logout is called THEN POST /api/logout is called', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);
    await logout();
    expect(fetchMock).toHaveBeenCalledWith('/api/logout', { credentials: 'include', method: 'POST' });
  });

  it('WHEN login is called THEN credentials are posted to /api/login', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ username: 'admin', role: 'global_admin' }) });
    vi.stubGlobal('fetch', fetchMock);
    await login('admin', 'secret');
    expect(fetchMock).toHaveBeenCalledWith('/api/login', {
      body: JSON.stringify({ username: 'admin', password: 'secret' }),
      credentials: 'include',
      headers: { 'Content-Type': 'application/json' },
      method: 'POST',
    });
  });

  it('WHEN data-fetch functions are called THEN they call the correct GET endpoints', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => [] });
    vi.stubGlobal('fetch', fetchMock);

    await fetchAgents();
    await fetchPendingEnrollments();
    await fetchLatestMetrics();
    await fetchGroups();
    await fetchUsers();
    await fetchUnacknowledgedAlerts();

    expect(fetchMock).toHaveBeenNthCalledWith(1, '/api/agents', { credentials: 'include' });
    expect(fetchMock).toHaveBeenNthCalledWith(2, '/api/pending-enrollments', { credentials: 'include' });
    expect(fetchMock).toHaveBeenNthCalledWith(3, '/api/metrics', { credentials: 'include' });
    expect(fetchMock).toHaveBeenNthCalledWith(4, '/api/groups', { credentials: 'include' });
    expect(fetchMock).toHaveBeenNthCalledWith(5, '/api/users', { credentials: 'include' });
    expect(fetchMock).toHaveBeenNthCalledWith(6, '/api/alerts/unacknowledged', { credentials: 'include' });
  });
});

describe('GIVEN API error handling', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('WHEN the server returns a 4xx status THEN the function throws with status info', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: false, status: 403, statusText: 'Forbidden' });
    vi.stubGlobal('fetch', fetchMock);
    await expect(fetchAgents()).rejects.toThrow('403 Forbidden');
  });

  it('WHEN the server returns a 5xx status THEN the function throws with status info', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: false, status: 500, statusText: 'Internal Server Error' });
    vi.stubGlobal('fetch', fetchMock);
    await expect(fetchSession()).rejects.toThrow('500 Internal Server Error');
  });

  it('WHEN a mutating endpoint returns a 401 THEN the function throws', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: false, status: 401, statusText: 'Unauthorized' });
    vi.stubGlobal('fetch', fetchMock);
    await expect(acknowledgeAlert(1)).rejects.toThrow('401 Unauthorized');
  });
});

describe('GIVEN loadDashboardData', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('WHEN called THEN it fetches all required endpoints and returns combined data', async () => {
    const fetchMock = vi.fn().mockImplementation((url: string) => {
      if (url === '/api/agents') return Promise.resolve({ ok: true, json: async () => [{ agent_id: 'a1' }] });
      if (url === '/api/pending-enrollments') return Promise.resolve({ ok: true, json: async () => [{ agent_id: 'p1' }] });
      if (url === '/api/metrics') return Promise.resolve({ ok: true, json: async () => [] });
      if (url === '/api/groups') return Promise.resolve({ ok: true, json: async () => [{ group_id: 1, name: 'All' }] });
      if (url === '/api/alerts/unacknowledged') return Promise.resolve({ ok: true, json: async () => [] });
      if (url === '/api/alerts') return Promise.resolve({ ok: true, json: async () => [] });
      if (url === '/api/users') return Promise.resolve({ ok: true, json: async () => [] });
      if (url === '/api/maintenance-windows') return Promise.resolve({ ok: true, json: async () => [] });
      if (url === '/api/silences') return Promise.resolve({ ok: true, json: async () => [{ silence_id: 7 }] });
      return Promise.resolve({ ok: true, json: async () => [] });
    });
    vi.stubGlobal('fetch', fetchMock);

    const data = await loadDashboardData();

    expect(data.agents).toEqual([{ agent_id: 'a1' }]);
    expect(data.pending).toEqual([{ agent_id: 'p1' }]);
    expect(data.groups).toEqual([{ group_id: 1, name: 'All' }]);
    expect(Array.isArray(data.alerts)).toBe(true);
    expect(Array.isArray(data.maintenanceWindows)).toBe(true);
    expect(data.silences).toEqual([{ silence_id: 7 }]);
  });

  it('WHEN optional endpoints fail THEN loadDashboardData still resolves with empty arrays for those fields', async () => {
    const fetchMock = vi.fn().mockImplementation((url: string) => {
      if (url === '/api/agents') return Promise.resolve({ ok: true, json: async () => [] });
      if (url === '/api/metrics') return Promise.resolve({ ok: true, json: async () => [] });
      return Promise.resolve({ ok: false, status: 403, statusText: 'Forbidden' });
    });
    vi.stubGlobal('fetch', fetchMock);

    const data = await loadDashboardData();

    expect(data.agents).toEqual([]);
    expect(data.pending).toEqual([]);
    expect(data.groups).toEqual([]);
    expect(data.alerts).toEqual([]);
    expect(data.users).toEqual([]);
    expect(data.maintenanceWindows).toEqual([]);
    expect(data.silences).toEqual([]);
  });
});

describe('GIVEN silence rule API functions', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('WHEN fetchSilences is called THEN it GETs /api/silences and returns the array', async () => {
    const silenceList = [{ silence_id: 1, agent_id: '*', indicator: '*', reason: 'planned', until_ms: 9999999, created_by: 'admin', created_at: 1000 }];
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => silenceList });
    vi.stubGlobal('fetch', fetchMock);

    const result = await fetchSilences();

    expect(fetchMock).toHaveBeenCalledWith('/api/silences', expect.objectContaining({ credentials: 'include' }));
    expect(result).toEqual(silenceList);
  });

  it('WHEN createSilence is called THEN it POSTs the correct payload to /api/silences', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ silence_id: 5 }) });
    vi.stubGlobal('fetch', fetchMock);

    await createSilence('agent-x', 'cpu', 'test silence', 9_000_000);

    const call = fetchMock.mock.calls[0];
    expect(call[0]).toBe('/api/silences');
    const body = JSON.parse(call[1].body as string);
    expect(body.agent_id).toBe('agent-x');
    expect(body.indicator).toBe('cpu');
    expect(body.reason).toBe('test silence');
    expect(body.until_ms).toBe(9_000_000);
  });

  it('WHEN deleteSilence is called THEN it sends DELETE to /api/silences/:id', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => ({ ok: true }) });
    vi.stubGlobal('fetch', fetchMock);

    await deleteSilence(42);

    expect(fetchMock).toHaveBeenCalledWith('/api/silences/42', expect.objectContaining({ method: 'DELETE' }));
  });
});

describe('GIVEN fetchAgentHistory', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('WHEN called THEN it GETs /api/agents/:id/history with default limit', async () => {
    const historyRows = [{ id: 1, agent_id: 'a1', indicator: 'cpu', old_status: 'green', new_status: 'amber', message: 'cpu degraded', created_at: 5000 }];
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => historyRows });
    vi.stubGlobal('fetch', fetchMock);

    const result = await fetchAgentHistory('my-agent');

    expect(fetchMock).toHaveBeenCalledWith('/api/agents/my-agent/history?limit=100', expect.anything());
    expect(result).toEqual(historyRows);
  });

  it('WHEN called with a custom limit THEN the limit is included in the query string', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => [] });
    vi.stubGlobal('fetch', fetchMock);

    await fetchAgentHistory('my-agent', 50);

    expect(fetchMock).toHaveBeenCalledWith('/api/agents/my-agent/history?limit=50', expect.anything());
  });

  it('WHEN the agent id contains special characters THEN it is URL-encoded', async () => {
    const fetchMock = vi.fn().mockResolvedValue({ ok: true, json: async () => [] });
    vi.stubGlobal('fetch', fetchMock);

    await fetchAgentHistory('agent/with spaces', 10);

    expect(fetchMock).toHaveBeenCalledWith('/api/agents/agent%2Fwith%20spaces/history?limit=10', expect.anything());
  });
});
