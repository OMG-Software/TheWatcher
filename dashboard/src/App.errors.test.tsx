import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { App } from './App';
import { RunbooksPage } from './pages/RunbooksPage';
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
    fireEvent.click(ackModal().parentElement as HTMLElement); // the backdrop
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
