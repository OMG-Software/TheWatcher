import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { App } from './App';
import { FakeApi, fail, ok } from './testing/fakeApi';
import { safeRunbookUrl } from './uiLogic';

async function openDashboard(api: FakeApi) {
  api.install();
  render(<App />);
  await screen.findByRole('navigation', { name: 'Dashboard views' });
}

const goTo = (label: string) => fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name: label }));

// The kinds the server lists at GET /api/indicators.
const kinds = [
  { name: 'cpu', label: 'CPU', qualifier: '' },
  { name: 'memory', label: 'Memory', qualifier: '' },
  { name: 'disk', label: 'Disk (every mount point)', qualifier: 'mount point' },
  { name: 'network', label: 'Network (every interface)', qualifier: 'interface' },
  { name: 'process', label: 'Watched processes', qualifier: 'process' },
  { name: 'Heartbeat', label: 'Agent offline (heartbeat)', qualifier: '' },
];

const alertRecord = (extra: object) => ({
  alert_id: 9, agent_id: 'agent-1', indicator: 'disk:/', old_status: 'green', new_status: 'red', message: 'disk full',
  created_at: 1000, acknowledged_by: '', acknowledged_at: 0, deleted_at: 0, note: '', escalated_at: 0, ...extra,
});

const alertsApi = (extra: object) =>
  new FakeApi('global_admin')
    .on('GET /api/alerts', ok([alertRecord(extra)]))
    .on('GET /api/alerts/unacknowledged', ok([alertRecord(extra)]));

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('GIVEN runbook URLs from the server', () => {
  it('WHEN the URL is http or https THEN it is returned trimmed', () => {
    expect(safeRunbookUrl(' https://wiki.example.com/x ')).toBe('https://wiki.example.com/x');
    expect(safeRunbookUrl('http://wiki.local/x')).toBe('http://wiki.local/x');
  });

  it('WHEN the URL is javascript:, data: or relative THEN nothing is returned', () => {
    expect(safeRunbookUrl('javascript:alert(1)')).toBeNull();
    expect(safeRunbookUrl('JaVaScRiPt:alert(1)')).toBeNull();
    expect(safeRunbookUrl('data:text/html,x')).toBeNull();
    expect(safeRunbookUrl('/runbooks/cpu')).toBeNull();
    expect(safeRunbookUrl(undefined)).toBeNull();
    expect(safeRunbookUrl('mailto:ops@example.com')).toBeNull();
  });
});

describe('GIVEN an alert with a runbook link', () => {
  it('WHEN the Alerts page shows it THEN the runbook link opens the URL in a new tab', async () => {
    await openDashboard(alertsApi({ runbook_url: 'https://wiki.example.com/disk' }));
    goTo('Alerts');

    const link = await screen.findByRole('link', { name: /Runbook link/ });
    expect(link).toHaveAttribute('href', 'https://wiki.example.com/disk');
    expect(link).toHaveAttribute('target', '_blank');
    expect(link).toHaveAttribute('rel', expect.stringContaining('noopener'));
  });

  it('WHEN the stored URL is javascript: THEN no link is rendered', async () => {
    await openDashboard(alertsApi({ runbook_url: 'javascript:alert(1)' }));
    goTo('Alerts');

    await screen.findByText('disk full');
    expect(screen.queryByRole('link', { name: /Runbook link/ })).toBeNull();
  });
});

describe('GIVEN the Silences page', () => {
  const openSilences = async (api: FakeApi) => {
    await openDashboard(api);
    goTo('Silences');
    await screen.findByLabelText('Reason');
  };
  const indicatorOptions = () => within(screen.getByLabelText('Indicator')).getAllByRole('option').map((o) => o.getAttribute('value'));

  it('WHEN the server lists indicator kinds THEN the picker offers exactly those and "*", not the old plural names', async () => {
    await openSilences(new FakeApi('global_admin').on('GET /api/indicators', ok(kinds)));

    await waitFor(() => expect(indicatorOptions()).toEqual(['*', 'cpu', 'memory', 'disk', 'network', 'process', 'Heartbeat']));
  });

  it('WHEN a per-mount kind is chosen with a mount point THEN the silence is created for the specific indicator', async () => {
    const api = new FakeApi('global_admin').on('GET /api/indicators', ok(kinds)).on('POST /api/silences', ok({ silence_id: 1 }));
    await openSilences(api);
    await waitFor(() => expect(indicatorOptions()).toContain('disk'));

    fireEvent.change(screen.getByLabelText('Indicator'), { target: { value: 'disk' } });
    fireEvent.change(screen.getByLabelText(/Mount point/), { target: { value: '/data' } });
    fireEvent.click(screen.getByRole('button', { name: /Add Silence Rule/ }));

    await waitFor(() => expect(api.count('POST /api/silences')).toBe(1));
    const post = api.calls.find((call) => call.method === 'POST' && call.path === '/api/silences');
    expect(post?.body).toMatchObject({ indicator: 'disk:/data' });
  });

  it('WHEN a per-mount kind is chosen without a mount point THEN the whole kind is silenced', async () => {
    const api = new FakeApi('global_admin').on('GET /api/indicators', ok(kinds)).on('POST /api/silences', ok({ silence_id: 1 }));
    await openSilences(api);
    await waitFor(() => expect(indicatorOptions()).toContain('disk'));

    fireEvent.change(screen.getByLabelText('Indicator'), { target: { value: 'disk' } });
    fireEvent.click(screen.getByRole('button', { name: /Add Silence Rule/ }));

    await waitFor(() => expect(api.count('POST /api/silences')).toBe(1));
    expect(api.calls.find((call) => call.path === '/api/silences' && call.method === 'POST')?.body).toMatchObject({ indicator: 'disk' });
  });

  it('WHEN the kinds cannot be loaded THEN the failure is shown and "All indicators" is still available', async () => {
    await openSilences(new FakeApi('global_admin').on('GET /api/indicators', fail(500, 'boom')));

    expect(await screen.findByText(/Could not load indicator names/)).toBeInTheDocument();
    expect(indicatorOptions()).toEqual(['*']);
  });
});

describe('GIVEN the Runbooks page', () => {
  it('WHEN the user is a group viewer THEN there is no Runbooks tab', async () => {
    await openDashboard(new FakeApi('group_viewer'));
    expect(within(screen.getByRole('navigation')).queryByRole('button', { name: 'Runbooks' })).toBeNull();
  });

  it('WHEN a runbook is added for one agent THEN the request names the agent', async () => {
    const api = new FakeApi('group_operator').on('POST /api/runbooks', ok({ runbook_id: 1 }));
    await openDashboard(api);
    goTo('Runbooks');

    fireEvent.change(await screen.findByLabelText('Agent'), { target: { value: 'agent-1' } });
    fireEvent.change(screen.getByLabelText('Runbook URL'), { target: { value: 'https://wiki.example.com/cpu' } });
    fireEvent.click(screen.getByRole('button', { name: /Add Runbook/ }));

    await waitFor(() => expect(api.count('POST /api/runbooks')).toBe(1));
    expect(api.calls.find((call) => call.method === 'POST')?.body).toMatchObject({
      agent_id: 'agent-1',
      url: 'https://wiki.example.com/cpu',
    });
  });

  it('WHEN a stored runbook URL is not http(s) THEN it is shown as text, not as a link', async () => {
    const api = new FakeApi('global_admin').on('GET /api/runbooks', ok([
      { runbook_id: 1, agent_id: '', indicator: 'cpu', status: 'red', url: 'javascript:alert(1)', markdown: '', notes: '', created_by: 'x', created_at: 1 },
    ]));
    await openDashboard(api);
    goTo('Runbooks');

    expect(await screen.findByText('javascript:alert(1)')).toBeInTheDocument();
    expect(screen.queryByRole('link', { name: 'javascript:alert(1)' })).toBeNull();
  });
});
