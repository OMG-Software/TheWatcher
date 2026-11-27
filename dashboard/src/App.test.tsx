import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { App } from './App';
import type { UserRole } from './models';
import { FakeApi, fail, ok } from './testing/fakeApi';

async function openDashboard(api: FakeApi) {
  api.install();
  render(<App />);
  await screen.findByRole('navigation', { name: 'Dashboard views' });
}

const goTo = (label: string) => fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name: label }));

afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

describe('GIVEN a signed-in user, role gating', () => {
  const tabs = (role: UserRole) => async () => {
    await openDashboard(new FakeApi(role));
    return within(screen.getByRole('navigation'))
      .getAllByRole('button')
      .map((button) => button.textContent);
  };

  it('WHEN the role is a group viewer THEN operator-only and settings tabs are hidden', async () => {
    const labels = await tabs('group_viewer')();

    expect(labels).not.toContain('Silences');
    expect(labels).not.toContain('Maintenance');
    expect(labels).not.toContain('Settings');
  });

  it('WHEN the role is a group operator THEN silences and maintenance show but settings stays hidden', async () => {
    const labels = await tabs('group_operator')();

    expect(labels).toContain('Silences');
    expect(labels).toContain('Maintenance');
    expect(labels).not.toContain('Settings');
  });

  it('WHEN the role is a global operator THEN settings is available', async () => {
    expect(await tabs('global_operator')()).toContain('Settings');
  });

  it('WHEN a viewer opens the agent list THEN the command buttons are disabled', async () => {
    await openDashboard(new FakeApi('global_viewer'));
    goTo('Agents');

    const restart = await screen.findByRole('button', { name: 'Restart' });

    expect(restart).toBeDisabled();
  });
});

describe('GIVEN an action that the server rejects', () => {
  it('WHEN the action fails THEN the page shows the server message', async () => {
    const api = new FakeApi('global_admin').on('POST /api/agents/agent-1/restart_collectors', fail(500, 'agent is not connected'));
    await openDashboard(api);
    goTo('Agents');

    fireEvent.click(await screen.findByRole('button', { name: 'Restart' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('agent is not connected');
  });

  it('WHEN the next action succeeds THEN the earlier error is cleared', async () => {
    const api = new FakeApi('global_admin').on('POST /api/agents/agent-1/restart_collectors', fail(500, 'agent is not connected'));
    await openDashboard(api);
    goTo('Agents');
    fireEvent.click(await screen.findByRole('button', { name: 'Restart' }));
    await screen.findByRole('alert');

    api.on('POST /api/agents/agent-1/restart_collectors', ok());
    fireEvent.click(screen.getByRole('button', { name: 'Restart' }));

    await waitFor(() => expect(screen.queryByRole('alert')).toBeNull());
  });
});

describe('GIVEN data that fails to load', () => {
  it('WHEN the agent list cannot be loaded THEN an error is shown, not "no agents"', async () => {
    const api = new FakeApi('global_admin').on('GET /api/agents', fail(500, 'store unavailable'));
    await openDashboard(api);

    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load dashboard data: store unavailable');
    expect(screen.queryByText(/No approved agents/)).toBeNull();
  });

  it('WHEN the alerts list cannot be loaded THEN the monitoring page says so instead of showing a clear board', async () => {
    const api = new FakeApi('global_admin').on('GET /api/alerts/unacknowledged', fail(500, 'database is locked'));
    await openDashboard(api);

    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load alerts: database is locked');
  });

  it('WHEN silences cannot be loaded THEN the silences page does not claim there are none', async () => {
    const api = new FakeApi('global_admin').on('GET /api/silences', fail(500, 'database is locked'));
    await openDashboard(api);
    goTo('Silences');

    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load silence rules');
    expect(screen.queryByText('No active silence rules.')).toBeNull();
  });
});

describe('GIVEN an expired session', () => {
  it('WHEN a poll returns 401 THEN the login screen shows with an expiry notice and polling stops', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval'] });
    const api = new FakeApi('global_admin');
    api.install();
    render(<App />);
    await screen.findByRole('navigation', { name: 'Dashboard views' });

    api.on('GET /api/agents', fail(401, 'authentication required'));
    fireEvent.click(screen.getByRole('button', { name: 'Refresh dashboard' }));

    expect(await screen.findByText(/session expired/i)).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Sign in' })).toBeInTheDocument();
    const callsAtExpiry = api.calls.length;
    await vi.advanceTimersByTimeAsync(30_000);

    expect(api.calls.length).toBe(callsAtExpiry);
  });

  it('WHEN the user logs out on purpose THEN no expiry notice is shown', async () => {
    const api = new FakeApi('global_admin');
    await openDashboard(api);

    fireEvent.click(screen.getByRole('button', { name: 'Log out' }));

    expect(await screen.findByRole('button', { name: 'Sign in' })).toBeInTheDocument();
    expect(screen.queryByText(/session expired/i)).toBeNull();
  });
});

describe('GIVEN the settings page', () => {
  it('WHEN settings fail to load THEN the form is not shown and defaults can never be saved', async () => {
    const api = new FakeApi('global_admin').on('GET /api/settings', fail(500, 'config unreadable'));
    await openDashboard(api);
    goTo('Settings');

    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load settings: config unreadable');
    expect(screen.queryByRole('button', { name: 'Save Settings' })).toBeNull();
    expect(api.calls.some((call) => call.method === 'PUT')).toBe(false);
  });

  it('WHEN settings load THEN the real values fill the form and an edit is saved', async () => {
    const settings = { webhook_url: 'https://hooks.example.com/x', offline_after_seconds: 45, escalation_timeout_seconds: 600, metrics_retention_days: 7, reports_enabled: false, reports_schedule: 'daily', reports_hour: 8, reports_day_of_week: 1, reports_webhook_url: '' };
    const api = new FakeApi('global_admin').on('GET /api/settings', ok(settings)).on('PUT /api/settings', ok());
    await openDashboard(api);
    goTo('Settings');

    fireEvent.change(await screen.findByLabelText('Offline after (seconds)'), { target: { value: '90' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save Settings' }));

    await waitFor(() => expect(api.calls.find((call) => call.method === 'PUT')?.body).toEqual({ ...settings, offline_after_seconds: 90 }));
  });

  it('WHEN a save fails THEN the edited values stay in the form with the server message', async () => {
    const settings = { webhook_url: '', offline_after_seconds: 45, escalation_timeout_seconds: 600, metrics_retention_days: 7, reports_enabled: false, reports_schedule: 'daily', reports_hour: 8, reports_day_of_week: 1, reports_webhook_url: '' };
    const api = new FakeApi('global_admin').on('GET /api/settings', ok(settings)).on('PUT /api/settings', fail(400, 'offline_after_seconds out of range'));
    await openDashboard(api);
    goTo('Settings');

    fireEvent.change(await screen.findByLabelText('Offline after (seconds)'), { target: { value: '50' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save Settings' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('offline_after_seconds out of range');
    expect(screen.getByLabelText('Offline after (seconds)')).toHaveValue(50);
  });
});

describe('GIVEN a form whose save fails', () => {
  it('WHEN adding a silence fails THEN the typed reason stays and the error is shown by the form', async () => {
    const api = new FakeApi('global_admin').on('POST /api/silences', fail(400, 'until_ms must be in the future'));
    await openDashboard(api);
    goTo('Silences');

    fireEvent.change(await screen.findByLabelText('Reason'), { target: { value: 'planned upgrade' } });
    fireEvent.click(screen.getByRole('button', { name: /Add Silence Rule/ }));

    expect(await screen.findByRole('alert')).toHaveTextContent('until_ms must be in the future');
    expect(screen.getByLabelText('Reason')).toHaveValue('planned upgrade');
  });

  it('WHEN adding a silence succeeds THEN the reason is cleared', async () => {
    const api = new FakeApi('global_admin').on('POST /api/silences', ok({ silence_id: 1 }));
    await openDashboard(api);
    goTo('Silences');

    fireEvent.change(await screen.findByLabelText('Reason'), { target: { value: 'planned upgrade' } });
    fireEvent.click(screen.getByRole('button', { name: /Add Silence Rule/ }));

    await waitFor(() => expect(screen.getByLabelText('Reason')).toHaveValue(''));
  });

  it('WHEN changing a password fails THEN the dialog stays open with the typed password', async () => {
    const api = new FakeApi('global_admin')
      .on('GET /api/users', ok([{ user_id: 2, username: 'bob', role: 'group_viewer', group_ids: [], disabled: false, built_in: false }]))
      .on('PUT /api/users/2/password', fail(400, 'password too short'));
    await openDashboard(api);
    goTo('Users');

    fireEvent.click(await screen.findAllByRole('button', { name: 'Password' }).then((buttons) => buttons[0]));
    fireEvent.change(screen.getByLabelText('New password'), { target: { value: 'abc' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('password too short');
    expect(screen.getByLabelText('New password')).toHaveValue('abc');
  });

  it('WHEN a global admin edits a user THEN only the changed role is sent and the dialog closes', async () => {
    const api = new FakeApi('global_admin')
      .on('GET /api/groups', ok([{ group_id: 1, name: 'Ops', built_in: false }]))
      .on('GET /api/users', ok([{ user_id: 2, username: 'bob', role: 'group_viewer', group_ids: [1], disabled: false, built_in: false }]))
      .on('PUT /api/users/2', ok());
    await openDashboard(api);
    goTo('Users');

    fireEvent.click(await screen.findByRole('button', { name: 'Edit' }));
    fireEvent.change(within(screen.getByRole('dialog', { name: 'Edit bob' })).getByLabelText('Account type'), { target: { value: 'group_operator' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save' }));

    await waitFor(() => expect(screen.queryByRole('dialog', { name: 'Edit bob' })).toBeNull());
    expect(api.calls.find((call) => call.method === 'PUT' && call.path === '/api/users/2')?.body).toEqual({ role: 'group_operator' });
  });

  it('WHEN the server refuses to demote the last global admin THEN the dialog stays open with the message', async () => {
    const api = new FakeApi('global_admin')
      .on('GET /api/users', ok([{ user_id: 2, username: 'bob', role: 'global_admin', group_ids: [], disabled: false, built_in: false }]))
      .on('PUT /api/users/2', fail(409, 'cannot remove or demote the last global admin'));
    await openDashboard(api);
    goTo('Users');

    fireEvent.click(await screen.findByRole('button', { name: 'Edit' }));
    fireEvent.change(within(screen.getByRole('dialog', { name: 'Edit bob' })).getByLabelText('Account type'), { target: { value: 'group_viewer' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('cannot remove or demote the last global admin');
    expect(screen.getByRole('dialog', { name: 'Edit bob' })).toBeInTheDocument();
  });

  it('WHEN a group admin views users THEN controls appear only for users inside their groups', async () => {
    const user = (id: number, name: string, role: string, group_ids: number[]) => ({ user_id: id, username: name, role, group_ids, disabled: false, built_in: false });
    const api = new FakeApi('group_admin')
      .on('GET /api/groups', ok([{ group_id: 1, name: 'Ops', built_in: false }]))
      .on('GET /api/users', ok([
        user(1, 'tester', 'group_admin', [1]),
        user(2, 'inside', 'group_viewer', [1]),
        user(3, 'shared', 'group_viewer', [1, 2]),
        user(4, 'global', 'global_operator', [1]),
      ]));
    await openDashboard(api);
    goTo('Users');

    const rowFor = async (name: string) => (await screen.findByText(name)).closest('tr') as HTMLElement;
    const inside = within(await rowFor('inside'));
    expect(inside.getByRole('button', { name: 'Edit' })).toBeInTheDocument();
    expect(inside.getByRole('button', { name: 'Disable' })).toBeInTheDocument();
    expect(inside.getByRole('button', { name: 'Delete' })).toBeInTheDocument();
    // The server only lets a global admin or the user change a password.
    expect(inside.queryByRole('button', { name: 'Password' })).toBeNull();

    const shared = within(await rowFor('shared'));
    expect(shared.getByRole('button', { name: 'Edit' })).toBeInTheDocument();
    expect(shared.queryByRole('button', { name: 'Disable' })).toBeNull();
    expect(shared.queryByRole('button', { name: 'Delete' })).toBeNull();

    const global = within(await rowFor('global'));
    expect(global.queryByRole('button', { name: 'Edit' })).toBeNull();
    expect(global.queryByRole('button', { name: 'Disable' })).toBeNull();
  });

  it('WHEN acknowledging an alert fails THEN the dialog stays open with the typed note', async () => {
    const alert = { alert_id: 9, agent_id: 'agent-1', indicator: 'cpu', old_status: 'green', new_status: 'red', message: 'cpu high', created_at: 1000, acknowledged_by: '', acknowledged_at: 0, deleted_at: 0, note: '', escalated_at: 0 };
    const api = new FakeApi('global_admin')
      .on('GET /api/alerts', ok([alert]))
      .on('GET /api/alerts/unacknowledged', ok([alert]))
      .on('POST /api/alerts/9/ack', fail(409, 'alert already archived'));
    await openDashboard(api);
    goTo('Alerts');

    fireEvent.click(await screen.findByRole('button', { name: 'Acknowledge' }));
    fireEvent.change(screen.getByPlaceholderText(/Describe what you found/), { target: { value: 'restarted the service' } });
    fireEvent.click(within(screen.getByRole('heading', { name: 'Acknowledge alert' }).closest('.modal-panel') as HTMLElement).getByRole('button', { name: 'Acknowledge' }));

    expect(await screen.findByRole('alert')).toHaveTextContent('alert already archived');
    expect(screen.getByPlaceholderText(/Describe what you found/)).toHaveValue('restarted the service');
  });
});

describe('GIVEN the alerts page with archived alerts shown', () => {
  it('WHEN the dashboard polls THEN archived alerts load once, not on every poll', async () => {
    vi.useFakeTimers({ toFake: ['setInterval', 'clearInterval'] });
    const api = new FakeApi('global_admin');
    await openDashboard(api);
    goTo('Alerts');
    fireEvent.click(await screen.findByLabelText('Show archived'));
    await waitFor(() => expect(api.count('GET /api/alerts?include_archived=1')).toBe(1));

    await vi.advanceTimersByTimeAsync(5000);

    // The dashboard poll ran again, but it did not trigger an archived re-fetch of its own:
    // only the archived view's own 5 s poll adds a request.
    expect(api.count('GET /api/agents')).toBeGreaterThanOrEqual(2);
    expect(api.count('GET /api/alerts?include_archived=1')).toBe(2);
  });
});
