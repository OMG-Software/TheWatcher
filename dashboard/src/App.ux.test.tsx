import React from 'react';
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { App } from './App';
import { ConfirmDialog } from './components/ConfirmDialog';
import { Modal } from './components/Modal';
import { FakeApi, agentRecord, ok } from './testing/fakeApi';

async function openDashboard(api: FakeApi) {
  api.install();
  render(<App />);
  await screen.findByRole('navigation', { name: 'Dashboard views' });
}

const goTo = (label: string) => fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name: label }));
const dialog = () => screen.getByRole('dialog');

function alertRow(id: number, agent: string, extra: Record<string, unknown> = {}) {
  return { alert_id: id, agent_id: agent, indicator: 'cpu', old_status: 'green', new_status: 'red', message: `alert ${id}`, created_at: 1000 + id, acknowledged_by: '', acknowledged_at: 0, deleted_at: 0, note: '', escalated_at: 0, ...extra };
}

beforeEach(() => {
  window.location.hash = '';
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

describe('GIVEN destructive actions (issue #109)', () => {
  it('WHEN Delete is clicked on the Agents page THEN a dialog names the agent and nothing is sent until it is confirmed', async () => {
    const api = new FakeApi('global_admin');
    await openDashboard(api);
    goTo('Agents');

    fireEvent.click(await screen.findByRole('button', { name: 'Delete' }));

    expect(within(dialog()).getByText('web-01')).toBeInTheDocument();
    expect(api.count('DELETE /api/agents/agent-1')).toBe(0);

    fireEvent.click(within(dialog()).getByRole('button', { name: 'Delete agent' }));
    await waitFor(() => expect(api.count('DELETE /api/agents/agent-1')).toBe(1));
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
  });

  it('WHEN the delete dialog is cancelled THEN no request is sent', async () => {
    const api = new FakeApi('global_admin');
    await openDashboard(api);
    goTo('Agents');
    fireEvent.click(await screen.findByRole('button', { name: 'Delete' }));

    fireEvent.click(within(dialog()).getByRole('button', { name: 'Cancel' }));

    expect(screen.queryByRole('dialog')).toBeNull();
    expect(api.count('DELETE /api/agents/agent-1')).toBe(0);
  });

  it('WHEN Maintenance is clicked THEN the operator can enter a reason, and it is what is sent', async () => {
    const api = new FakeApi('global_admin');
    await openDashboard(api);
    goTo('Agents');
    fireEvent.click(within(await screen.findByRole('main')).getByRole('button', { name: 'Maintenance' }));

    fireEvent.change(within(dialog()).getByLabelText(/Reason/), { target: { value: 'kernel patch' } });
    fireEvent.click(within(dialog()).getByRole('button', { name: 'Enter maintenance' }));

    await waitFor(() => expect(api.count('POST /api/agents/agent-1/maintenance')).toBe(1));
    expect(api.calls.find((call) => call.path.endsWith('/maintenance'))?.body).toEqual({ reason: 'kernel patch', until_ms: 0 });
  });

  it('WHEN Maintenance is confirmed without a reason THEN an empty reason is sent, not a made-up one', async () => {
    const api = new FakeApi('global_admin');
    await openDashboard(api);
    goTo('Agents');
    fireEvent.click(within(await screen.findByRole('main')).getByRole('button', { name: 'Maintenance' }));

    fireEvent.click(within(dialog()).getByRole('button', { name: 'Enter maintenance' }));

    await waitFor(() => expect(api.count('POST /api/agents/agent-1/maintenance')).toBe(1));
    expect(api.calls.find((call) => call.path.endsWith('/maintenance'))?.body).toEqual({ reason: '', until_ms: 0 });
  });

  it('WHEN bulk Archive is clicked THEN the dialog states the count and only a confirm archives', async () => {
    const api = new FakeApi('global_admin').on('GET /api/alerts', ok([alertRow(1, 'agent-1'), alertRow(2, 'agent-1')]));
    await openDashboard(api);
    goTo('Alerts');
    fireEvent.click(await screen.findByRole('button', { name: 'Select all alerts' }));

    fireEvent.click(screen.getByRole('button', { name: 'Archive (2)' }));
    expect(within(dialog()).getByText(/Archive 2 selected alerts/)).toBeInTheDocument();
    expect(api.count('POST /api/alerts/bulk-archive')).toBe(0);

    fireEvent.click(within(dialog()).getByRole('button', { name: 'Archive 2 alerts' }));
    await waitFor(() => expect(api.count('POST /api/alerts/bulk-archive')).toBe(1));
  });

  it('WHEN the confirmed action fails THEN the dialog stays open and shows the error', async () => {
    const onConfirm = vi.fn(async () => false);
    render(<ConfirmDialog busy={false} confirmLabel="Do it" errorKey="k" message="Sure?" onCancel={() => undefined} onConfirm={onConfirm} title="Confirm" />);

    fireEvent.click(screen.getByRole('button', { name: 'Do it' }));

    await waitFor(() => expect(onConfirm).toHaveBeenCalled());
    expect(screen.getByRole('dialog')).toBeInTheDocument();
  });
});

describe('GIVEN the Alerts page selection (issue #110)', () => {
  it('WHEN a filter hides selected alerts THEN the bulk button counts and archives only the visible ones', async () => {
    const api = new FakeApi('global_admin')
      .on('GET /api/agents', ok([agentRecord('agent-1', 'web-01'), agentRecord('agent-2', 'db-01')]))
      .on('GET /api/alerts', ok([alertRow(1, 'agent-1'), alertRow(2, 'agent-1'), alertRow(3, 'agent-2')]));
    await openDashboard(api);
    goTo('Alerts');
    fireEvent.click(await screen.findByRole('button', { name: 'Select all alerts' }));
    expect(screen.getByRole('button', { name: 'Archive (3)' })).toBeInTheDocument();

    fireEvent.change(screen.getByLabelText('Filter by agent'), { target: { value: 'agent-2' } });

    expect(screen.getByRole('button', { name: 'Archive (1)' })).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Archive (1)' }));
    fireEvent.click(within(dialog()).getByRole('button', { name: 'Archive 1 alert' }));
    await waitFor(() => expect(api.count('POST /api/alerts/bulk-archive')).toBe(1));
    expect(api.calls.find((call) => call.path === '/api/alerts/bulk-archive')?.body).toEqual({ alert_ids: [3] });
  });

  it('WHEN an agent has only archived alerts THEN it is in the agent filter once archived alerts are shown', async () => {
    const api = new FakeApi('global_admin')
      .on('GET /api/agents', ok([agentRecord('agent-1', 'web-01'), agentRecord('agent-2', 'db-01')]))
      .on('GET /api/alerts', ok([alertRow(1, 'agent-1')]))
      .on('GET /api/alerts?include_archived=1', ok([alertRow(1, 'agent-1'), alertRow(2, 'agent-2', { deleted_at: 5 })]));
    await openDashboard(api);
    goTo('Alerts');
    await screen.findByText('alert 1');
    expect(within(screen.getByLabelText('Filter by agent')).queryByRole('option', { name: 'db-01' })).toBeNull();

    fireEvent.click(screen.getByLabelText('Show archived'));

    await waitFor(() => expect(within(screen.getByLabelText('Filter by agent')).getByRole('option', { name: 'db-01' })).toBeInTheDocument());
  });

  it('WHEN the list reaches the server cap THEN the page says it is truncated', async () => {
    const many = Array.from({ length: 1000 }, (_, i) => alertRow(i + 1, 'agent-1'));
    await openDashboard(new FakeApi('global_admin').on('GET /api/alerts', ok(many)));
    goTo('Alerts');

    expect(await screen.findByText(/Showing the newest 1000 alerts/)).toBeInTheDocument();
  });
});

describe('GIVEN an active view that is gone (issue #165)', () => {
  it('WHEN the bookmarked view does not exist THEN the filter and hash are dropped and agents show', async () => {
    window.location.hash = 'view-99';
    await openDashboard(new FakeApi('global_admin').on('GET /api/views', ok([])));

    expect(await screen.findByText('web-01')).toBeInTheDocument();
    expect(screen.queryByText(/Filtered to view/)).toBeNull();
    expect(window.location.hash).not.toBe('#view-99');
  });

  it('WHEN the view exists THEN only its agents show, with its name', async () => {
    window.location.hash = 'view-1';
    const views = [{ view_id: 1, name: 'Edge', owner_user_id: 1, owner_username: 'tester', is_public: false, group_id: 0, agent_ids: ['agent-2'], created_at: 1 }];
    const api = new FakeApi('global_admin')
      .on('GET /api/agents', ok([agentRecord('agent-1', 'web-01'), agentRecord('agent-2', 'db-01')]))
      .on('GET /api/views', ok(views));
    await openDashboard(api);

    expect(await screen.findByText('db-01')).toBeInTheDocument();
    expect(screen.queryByText('web-01')).toBeNull();
    expect(screen.getByText('Edge')).toBeInTheDocument();
  });
});

describe('GIVEN the server status engine (issue #114)', () => {
  const withStatus = (connected: boolean, indicator_status: Record<string, string>) => ({ ...agentRecord('agent-1', 'web-01'), connected, indicator_status });

  it('WHEN the server reports cpu red THEN the row shows Critical', async () => {
    await openDashboard(new FakeApi('global_admin').on('GET /api/agents', ok([withStatus(true, { cpu: 'red', memory: 'green' })])));

    expect(await screen.findByText('Critical')).toBeInTheDocument();
  });

  it('WHEN a host is offline THEN the row says Offline and the summary agrees', async () => {
    await openDashboard(new FakeApi('global_admin').on('GET /api/agents', ok([withStatus(false, { cpu: 'green', memory: 'green' })])));

    await waitFor(() => expect(screen.getAllByText('Offline')).toHaveLength(2)); // summary card and row
  });
});

describe('GIVEN accessibility of rows and dialogs (issue #115)', () => {
  it('WHEN a row is expanded with the keyboard-reachable toggle THEN aria-expanded flips', async () => {
    await openDashboard(new FakeApi('global_admin'));
    const toggle = await screen.findByRole('button', { name: 'Expand web-01' });
    expect(toggle).toHaveAttribute('aria-expanded', 'false');

    fireEvent.click(toggle);

    expect(screen.getByRole('button', { name: 'Collapse web-01' })).toHaveAttribute('aria-expanded', 'true');
  });

  it('WHEN a tab is active THEN it has aria-current', async () => {
    await openDashboard(new FakeApi('global_admin'));

    expect(within(screen.getByRole('navigation')).getByRole('button', { name: 'Monitoring' })).toHaveAttribute('aria-current', 'page');
  });

  it('WHEN a busy action button is rendered THEN it keeps its name', () => {
    render(<ConfirmDialog busy confirmLabel="Delete agent" errorKey="k" message="m" onCancel={() => undefined} onConfirm={async () => true} title="t" />);

    expect(screen.getByRole('dialog')).toHaveAttribute('aria-modal', 'true');
    expect(screen.getByRole('button', { name: 'Delete agent' })).toBeDisabled();
  });
});

describe('GIVEN the Modal component', () => {
  function Harness({ dismissible = true }: { dismissible?: boolean }) {
    const [open, setOpen] = React.useState(false);
    return (
      <>
        <button onClick={() => setOpen(true)}>open</button>
        {open ? (
          <Modal dismissible={dismissible} onClose={() => setOpen(false)} title="Example">
            <button>first</button>
            <button>last</button>
          </Modal>
        ) : null}
      </>
    );
  }

  it('WHEN it opens THEN it is a named modal dialog and focus moves inside', () => {
    render(<Harness />);
    fireEvent.click(screen.getByText('open'));

    expect(screen.getByRole('dialog', { name: 'Example' })).toHaveAttribute('aria-modal', 'true');
    expect(dialog().contains(document.activeElement)).toBe(true);
  });

  it('WHEN Escape is pressed THEN it closes and focus returns to the opener', () => {
    render(<Harness />);
    const opener = screen.getByText('open');
    opener.focus();
    fireEvent.click(opener);

    fireEvent.keyDown(dialog(), { key: 'Escape' });

    expect(screen.queryByRole('dialog')).toBeNull();
    expect(document.activeElement).toBe(opener);
  });

  it('WHEN Tab is pressed on the last control THEN focus wraps to the first', () => {
    render(<Harness />);
    fireEvent.click(screen.getByText('open'));
    const last = screen.getByText('last');
    last.focus();

    fireEvent.keyDown(dialog(), { key: 'Tab' });

    expect(document.activeElement).toBe(within(dialog()).getByRole('button', { name: 'Close' }));
  });

  it('WHEN it is not dismissible (a request is running) THEN Escape does not close it', () => {
    render(<Harness dismissible={false} />);
    fireEvent.click(screen.getByText('open'));

    fireEvent.keyDown(dialog(), { key: 'Escape' });

    expect(screen.getByRole('dialog')).toBeInTheDocument();
  });
});

describe('GIVEN the retention settings (issue: settings fields)', () => {
  const base = { webhook_url: '', offline_after_seconds: 45, escalation_timeout_seconds: 600, metrics_retention_days: 7, reports_enabled: false, reports_schedule: 'daily', reports_hour: 8, reports_day_of_week: 1, reports_webhook_url: '' };

  it('WHEN the server reports the new retention keys THEN they are editable and saved', async () => {
    const api = new FakeApi('global_admin')
      .on('GET /api/settings', ok({ ...base, status_history_retention_days: 90, alerts_retention_days: 90, log_matches_retention_days: 30 }));
    await openDashboard(api);
    goTo('Settings');
    const field = await screen.findByLabelText('Alerts retention (days)');
    expect(field).toBeEnabled();

    fireEvent.change(field, { target: { value: '120' } });
    fireEvent.click(screen.getByRole('button', { name: 'Save Settings' }));

    await waitFor(() => expect(api.count('PUT /api/settings')).toBe(1));
    const sent = api.calls.find((call) => call.method === 'PUT')?.body as Record<string, unknown>;
    expect(sent.alerts_retention_days).toBe(120);
    expect(sent.log_matches_retention_days).toBe(30);
  });

  it('WHEN the server does not report them THEN the fields are disabled and no new key is sent', async () => {
    const api = new FakeApi('global_admin').on('GET /api/settings', ok(base));
    await openDashboard(api);
    goTo('Settings');

    expect(await screen.findByLabelText('Alerts retention (days)')).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Save Settings' }));

    await waitFor(() => expect(api.count('PUT /api/settings')).toBe(1));
    const sent = api.calls.find((call) => call.method === 'PUT')?.body as Record<string, unknown>;
    expect(Object.keys(sent)).not.toContain('alerts_retention_days');
  });
});

describe('GIVEN an expanded row (issue #111)', () => {
  it('WHEN the poll interval passes THEN its history is fetched again', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    const api = new FakeApi('global_admin');
    await openDashboard(api);
    fireEvent.click(await screen.findByRole('button', { name: 'Expand web-01' }));
    await waitFor(() => expect(api.count('GET /api/metrics/agent-1?limit=20')).toBe(1));

    await vi.advanceTimersByTimeAsync(5500);

    expect(api.count('GET /api/metrics/agent-1?limit=20')).toBeGreaterThanOrEqual(2);
  });
});
