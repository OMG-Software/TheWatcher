import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { App } from './App';
import { RunbooksPage } from './pages/RunbooksPage';
import { clickBackdrop } from './testing/dialog';
import { FakeApi, deferred, fail, ok, type FakeReply } from './testing/fakeApi';

async function openDashboard(api: FakeApi) {
  api.install();
  render(<App />);
  await screen.findByRole('navigation', { name: 'Dashboard views' });
}

const goTo = (label: string) => fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name: label }));

const alertRecord = { alert_id: 9, agent_id: 'agent-1', indicator: 'cpu', old_status: 'green', new_status: 'red', message: 'cpu high', created_at: 1000, acknowledged_by: '', acknowledged_at: 0, deleted_at: 0, note: '', escalated_at: 0 };
const settings = { webhook_url: '', offline_after_seconds: 45, escalation_timeout_seconds: 600, metrics_retention_days: 7, reports_enabled: false, reports_schedule: 'daily', reports_hour: 8, reports_day_of_week: 1, reports_webhook_url: '' };

const alertsApi = () => new FakeApi('global_admin').on('GET /api/alerts', ok([alertRecord])).on('GET /api/alerts/unacknowledged', ok([alertRecord]));
const ackModal = () => screen.getByRole('heading', { name: 'Acknowledge alert' }).closest('.modal-panel') as HTMLElement;

afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

describe('GIVEN agent history and log matches loading independently', () => {
  const expandRow = async () => {
    fireEvent.click(await screen.findByText('web-01'));
  };
  const logRow = { match_id: 1, agent_id: 'agent-1', indicator_name: 'syslog', severity: 'red', path: '/var/log/syslog', matched_line: 'disk failure', created_at: 1000 };

  it('WHEN only the metric history fails THEN the log matches still show and the error names the history', async () => {
    const api = new FakeApi('global_admin')
      .on('GET /api/metrics/agent-1?limit=20', fail(500, 'metrics db locked'))
      .on('GET /api/agents/agent-1/log-matches?limit=20', ok([logRow]));
    await openDashboard(api);
    await expandRow();

    expect(await screen.findByText(/Could not load metric history for web-01: metrics db locked/)).toBeInTheDocument();
    expect(await screen.findByText(/disk failure/)).toBeInTheDocument();
    expect(screen.queryByText(/Could not load log matches/)).toBeNull();
  });

  it('WHEN only the log matches fail THEN the history error is not shown and the log error names the matches', async () => {
    const api = new FakeApi('global_admin')
      .on('GET /api/metrics/agent-1?limit=20', ok([]))
      .on('GET /api/agents/agent-1/log-matches?limit=20', fail(500, 'log db locked'));
    await openDashboard(api);
    await expandRow();

    expect(await screen.findByText(/Could not load log matches for web-01: log db locked/)).toBeInTheDocument();
    expect(screen.queryByText(/Could not load metric history/)).toBeNull();
  });
});

describe('GIVEN a settings save that is still in flight', () => {
  it('WHEN the save is pending THEN the form is disabled so no edit can be lost, and it re-enables afterwards', async () => {
    const reply = deferred<FakeReply>();
    const api = new FakeApi('global_admin').on('GET /api/settings', ok(settings)).on('PUT /api/settings', () => reply.promise);
    await openDashboard(api);
    goTo('Settings');
    const offline = await screen.findByLabelText('Offline after (seconds)');
    fireEvent.change(offline, { target: { value: '90' } });

    fireEvent.click(screen.getByRole('button', { name: 'Save Settings' }));

    expect(screen.getByLabelText('Offline after (seconds)')).toBeDisabled();
    reply.resolve(ok());
    await waitFor(() => expect(screen.getByLabelText('Offline after (seconds)')).toBeEnabled());
  });
});

describe('GIVEN a settings save whose reload fails', () => {
  it('WHEN the save succeeds but the reload fails THEN the saved values stay visible with a refresh warning', async () => {
    let gets = 0;
    const api = new FakeApi('global_admin')
      .on('GET /api/settings', () => (++gets === 1 ? ok(settings) : fail(500, 'config unreadable')))
      .on('PUT /api/settings', ok());
    await openDashboard(api);
    goTo('Settings');
    fireEvent.change(await screen.findByLabelText('Offline after (seconds)'), { target: { value: '90' } });

    fireEvent.click(screen.getByRole('button', { name: 'Save Settings' }));

    expect(await screen.findByText(/Could not refresh settings: config unreadable/)).toBeInTheDocument();
    expect(screen.getByLabelText('Offline after (seconds)')).toHaveValue(90);
  });
});

describe('GIVEN a dialog whose request is still running', () => {
  const startAck = async (api: FakeApi) => {
    await openDashboard(api);
    goTo('Alerts');
    fireEvent.click(await screen.findByRole('button', { name: 'Acknowledge' }));
    fireEvent.change(screen.getByPlaceholderText(/Describe what you found/), { target: { value: 'restarted' } });
    fireEvent.click(within(ackModal()).getByRole('button', { name: 'Acknowledge' }));
  };

  it('WHEN the user tries to dismiss it THEN it stays open and the late failure is shown inside it', async () => {
    const reply = deferred<FakeReply>();
    await startAck(alertsApi().on('POST /api/alerts/9/ack', () => reply.promise));

    expect(within(ackModal()).getByRole('button', { name: 'Cancel' })).toBeDisabled();
    fireEvent.click(within(ackModal()).getByRole('button', { name: 'Cancel' }));
    clickBackdrop(ackModal());
    expect(screen.getByRole('heading', { name: 'Acknowledge alert' })).toBeInTheDocument();

    reply.resolve(fail(409, 'alert already archived'));
    expect(await within(ackModal()).findByRole('alert')).toHaveTextContent('alert already archived');
  });

  it('WHEN the agent configuration is saving THEN its Close and Cancel buttons are disabled', async () => {
    const reply = deferred<FakeReply>();
    const api = new FakeApi('global_admin').on('POST /api/agents/agent-1/collector_config', () => reply.promise);
    await openDashboard(api);
    goTo('Agents');
    fireEvent.click(await screen.findByRole('button', { name: 'Configure' }));

    fireEvent.click(screen.getByRole('button', { name: 'Save' }));

    expect(screen.getByRole('button', { name: 'Close configuration' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Cancel' })).toBeDisabled();
    reply.resolve(ok());
    await waitFor(() => expect(screen.queryByRole('button', { name: 'Close configuration' })).toBeNull());
  });
});

describe('GIVEN a dialog that showed a failure', () => {
  it('WHEN the alert dialog is cancelled and reopened THEN the old error is gone', async () => {
    const api = alertsApi().on('POST /api/alerts/9/ack', fail(409, 'alert already archived'));
    await openDashboard(api);
    goTo('Alerts');
    fireEvent.click(await screen.findByRole('button', { name: 'Acknowledge' }));
    fireEvent.click(within(ackModal()).getByRole('button', { name: 'Acknowledge' }));
    await screen.findByText('alert already archived');

    fireEvent.click(within(ackModal()).getByRole('button', { name: 'Cancel' }));
    fireEvent.click(await screen.findByRole('button', { name: 'Acknowledge' }));

    expect(screen.queryByText('alert already archived')).toBeNull();
  });

  it('WHEN the password dialog is cancelled and reopened THEN the old error is gone', async () => {
    const api = new FakeApi('global_admin')
      .on('GET /api/users', ok([{ user_id: 2, username: 'bob', role: 'group_viewer', group_ids: [], disabled: false, built_in: false }]))
      .on('PUT /api/users/2/password', fail(400, 'password too short'));
    await openDashboard(api);
    goTo('Users');
    fireEvent.click((await screen.findAllByRole('button', { name: 'Password' }))[0]);
    fireEvent.change(screen.getByLabelText('New password'), { target: { value: 'abc' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save' }));
    await screen.findByText('password too short');

    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    fireEvent.click((await screen.findAllByRole('button', { name: 'Password' }))[0]);

    expect(screen.queryByText('password too short')).toBeNull();
  });

  it('WHEN the agent configuration is cancelled and reopened THEN the old error is gone', async () => {
    const api = new FakeApi('global_admin').on('POST /api/agents/agent-1/collector_config', fail(400, 'interval out of range'));
    await openDashboard(api);
    goTo('Agents');
    fireEvent.click(await screen.findByRole('button', { name: 'Configure' }));
    fireEvent.click(screen.getByRole('button', { name: 'Save' }));
    await screen.findByText('interval out of range');

    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    fireEvent.click(await screen.findByRole('button', { name: 'Configure' }));

    expect(screen.queryByText('interval out of range')).toBeNull();
  });
});

describe('GIVEN groups that fail to load', () => {
  it.each(['Monitoring', 'Views'])('WHEN the %s page shows THEN it says groups could not be loaded', async (page) => {
    const api = new FakeApi('global_admin').on('GET /api/groups', fail(500, 'groups db locked'));
    await openDashboard(api);
    if (page !== 'Monitoring') goTo(page);

    expect(await screen.findByText(/Could not load groups: groups db locked/)).toBeInTheDocument();
  });
});

describe('GIVEN the runbooks page before its first load settles', () => {
  it('WHEN the request is pending THEN it shows loading, not "No runbooks configured."', async () => {
    const reply = deferred<FakeReply>();
    new FakeApi('global_admin').on('GET /api/runbooks', () => reply.promise).install();
    render(<RunbooksPage runAction={vi.fn()} />);

    expect(screen.getByText(/Loading runbooks/)).toBeInTheDocument();
    expect(screen.queryByText('No runbooks configured.')).toBeNull();

    reply.resolve(ok([]));
    expect(await screen.findByText('No runbooks configured.')).toBeInTheDocument();
  });
});

describe('GIVEN a successful settings save', () => {
  it('WHEN the page reloads THEN settings are fetched exactly once more, and a failing reload keeps the saved values', async () => {
    let gets = 0;
    const api = new FakeApi('global_admin')
      .on('GET /api/settings', () => (++gets === 1 ? ok(settings) : fail(500, 'config unreadable')))
      .on('PUT /api/settings', ok());
    await openDashboard(api);
    goTo('Settings');
    fireEvent.change(await screen.findByLabelText('Offline after (seconds)'), { target: { value: '90' } });

    fireEvent.click(screen.getByRole('button', { name: 'Save Settings' }));
    await screen.findByText(/Could not refresh settings: config unreadable/);

    expect(api.count('GET /api/settings')).toBe(2); // the first load plus one reload, no extra
    expect(screen.getByLabelText('Offline after (seconds)')).toHaveValue(90);
  });
});

describe('GIVEN the archived alerts list', () => {
  const ARCHIVED = 'GET /api/alerts?include_archived=1';
  const showArchived = () => fireEvent.click(screen.getByLabelText('Show archived'));
  const openAlerts = async (api: FakeApi) => {
    await openDashboard(api);
    goTo('Alerts');
    await screen.findByRole('heading', { name: 'Alerts' });
  };

  it('WHEN the request is still pending THEN it shows loading and no empty-list text', async () => {
    const reply = deferred<FakeReply>();
    await openAlerts(alertsApi().on(ARCHIVED, () => reply.promise));

    showArchived();

    expect(await screen.findByText(/Loading archived alerts/)).toBeInTheDocument();
    expect(screen.queryByText('No alerts.')).toBeNull();
    reply.resolve(ok([]));
    expect(await screen.findByText('No alerts.')).toBeInTheDocument();
  });

  it('WHEN the request fails THEN the table says the list is unavailable instead of empty', async () => {
    await openAlerts(alertsApi().on(ARCHIVED, fail(500, 'archive db locked')));

    showArchived();

    expect(await screen.findByText(/Could not load archived alerts: archive db locked/)).toBeInTheDocument();
    expect(within(screen.getByRole('table')).getByText('Archived alerts are unavailable.')).toBeInTheDocument();
    expect(screen.queryByText('No alerts.')).toBeNull();
  });

  it('WHEN it loads and has no alerts THEN the empty-list text shows', async () => {
    await openAlerts(alertsApi().on(ARCHIVED, ok([])));

    showArchived();

    expect(await screen.findByText('No alerts.')).toBeInTheDocument();
    expect(screen.queryByText('Archived alerts are unavailable.')).toBeNull();
  });

  it('WHEN an alert is acknowledged while the list is shown THEN the list is reloaded and shows the new state', async () => {
    let acknowledged = false;
    const api = alertsApi()
      .on(ARCHIVED, () => ok([{ ...alertRecord, acknowledged_by: acknowledged ? 'tester' : '', acknowledged_at: acknowledged ? 2000 : 0 }]))
      .on('POST /api/alerts/9/ack', () => {
        acknowledged = true;
        return ok();
      });
    await openAlerts(api);
    showArchived();
    fireEvent.click(await screen.findByRole('button', { name: 'Acknowledge' }));

    fireEvent.click(within(ackModal()).getByRole('button', { name: 'Acknowledge' }));

    // The list polls only every 5 s, so seeing the change now means the action reloaded it.
    expect(await screen.findByRole('button', { name: 'Archive' })).toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'Acknowledge' })).toBeNull();
  });
});

describe('GIVEN a view whose save is still running', () => {
  const view = (id: number, name: string) => ({ view_id: id, name, owner_user_id: 1, owner_username: 'tester', is_public: false, group_id: 0, agent_ids: [], created_at: 1000 });

  it('WHEN another action starts meanwhile THEN Cancel stays disabled until the save settles', async () => {
    const save = deferred<FakeReply>();
    const removal = deferred<FakeReply>();
    const api = new FakeApi('global_admin')
      .on('GET /api/views', ok([view(1, 'first'), view(2, 'second')]))
      .on('PUT /api/views/1', () => save.promise)
      .on('DELETE /api/views/2', () => removal.promise);
    await openDashboard(api);
    fireEvent.click(await within(screen.getByRole('navigation')).findByRole('button', { name: /^Views/ }));
    fireEvent.click((await screen.findAllByRole('button', { name: 'Edit' }))[0]);
    fireEvent.click(screen.getByRole('button', { name: 'Save' }));

    fireEvent.click(await screen.findByRole('button', { name: 'Delete' }));
    fireEvent.click(await screen.findByRole('button', { name: 'Delete view' })); // confirming starts a newer action

    await waitFor(() => expect(api.count('DELETE /api/views/2')).toBe(1));
    const rowCancel = within(screen.getByRole('table')).getByRole('button', { name: 'Cancel', hidden: true });
    expect(rowCancel).toBeDisabled();
    save.resolve(ok());
    removal.resolve(ok());
    await waitFor(() => expect(screen.queryByRole('button', { name: 'Cancel', hidden: true })).toBeNull());
  });
});

describe('GIVEN settings where only one field was edited', () => {
  it('WHEN the save succeeds, the reload fails and another admin changes a different field THEN Retry shows the new server value and keeps the edit', async () => {
    let gets = 0;
    const api = new FakeApi('global_admin')
      .on('GET /api/settings', () => {
        gets += 1;
        if (gets === 1) return ok(settings);
        if (gets === 2) return fail(500, 'config unreadable');
        return ok({ ...settings, webhook_url: 'https://hooks.example.com/a', metrics_retention_days: 90 });
      })
      .on('PUT /api/settings', ok());
    await openDashboard(api);
    goTo('Settings');
    fireEvent.change(await screen.findByLabelText('Webhook URL'), { target: { value: 'https://hooks.example.com/a' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save Settings' }));
    await screen.findByText(/Could not refresh settings: config unreadable/);

    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));

    await waitFor(() => expect(screen.getByLabelText('Metrics retention (days)')).toHaveValue(90));
    expect(screen.getByLabelText('Webhook URL')).toHaveValue('https://hooks.example.com/a');
  });

  it('WHEN a field is edited and saved THEN the unedited fields are sent with their server values', async () => {
    const api = new FakeApi('global_admin').on('GET /api/settings', ok(settings)).on('PUT /api/settings', ok());
    await openDashboard(api);
    goTo('Settings');
    fireEvent.change(await screen.findByLabelText('Offline after (seconds)'), { target: { value: '90' } });

    fireEvent.click(screen.getByRole('button', { name: 'Save Settings' }));

    await waitFor(() => expect(api.count('PUT /api/settings')).toBe(1));
    expect(api.calls.find((call) => call.method === 'PUT')?.body).toEqual({ ...settings, offline_after_seconds: 90 });
  });
});

describe('GIVEN a sign-out the server does not confirm', () => {
  it('WHEN the request fails THEN the dashboard stays, says the session is still active, and a retry signs out', async () => {
    const api = new FakeApi('global_admin').on('POST /api/logout', fail(500, 'session store down'));
    await openDashboard(api);

    fireEvent.click(screen.getByRole('button', { name: 'Log out' }));

    const alert = await screen.findByRole('alert');
    expect(alert).toHaveTextContent(/Sign.?out failed/i);
    expect(alert).toHaveTextContent('session store down');
    expect(alert).toHaveTextContent(/still active/i);
    expect(screen.getByRole('navigation', { name: 'Dashboard views' })).toBeInTheDocument();

    api.on('POST /api/logout', ok());
    fireEvent.click(within(alert).getByRole('button', { name: 'Try again' }));

    expect(await screen.findByRole('button', { name: 'Sign in' })).toBeInTheDocument();
  });

  it('WHEN the server answers 401 THEN the session was already over and the login screen shows without an error', async () => {
    const api = new FakeApi('global_admin').on('POST /api/logout', fail(401, 'authentication required'));
    await openDashboard(api);

    fireEvent.click(screen.getByRole('button', { name: 'Log out' }));

    expect(await screen.findByRole('button', { name: 'Sign in' })).toBeInTheDocument();
    expect(screen.queryByText(/still active/i)).toBeNull();
  });
});
