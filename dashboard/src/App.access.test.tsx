import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { App } from './App';
import type { UserRole } from './models';
import { FakeApi, agentRecord, fail, ok } from './testing/fakeApi';

const GROUPS = [
  { group_id: 1, name: 'Prod', built_in: false },
  { group_id: 2, name: 'Dev', built_in: false },
];

const view = (viewId: number, name: string, groupId: number, owner = 'someone-else') => ({
  view_id: viewId,
  name,
  owner_user_id: 9,
  owner_username: owner,
  is_public: false,
  group_id: groupId,
  agent_ids: [],
  created_at: 1,
});

async function openDashboard(api: FakeApi) {
  api.install();
  render(<App />);
  await screen.findByRole('navigation', { name: 'Dashboard views' });
}

// The Views tab gains a count once views load ("Views (3)"), so match by prefix.
const goTo = (label: string) => fireEvent.click(within(screen.getByRole('navigation')).getByRole('button', { name: new RegExp(`^${label}`) }));

const apiFor = (role: UserRole) => new FakeApi(role).on('GET /api/groups', ok(GROUPS));

afterEach(() => {
  vi.unstubAllGlobals();
  window.location.hash = '';
});

describe('GIVEN a pending enrollment and two groups', () => {
  const pendingApi = () => apiFor('global_admin').on('GET /api/pending-enrollments', ok([{ ...agentRecord('new-host', 'new-host'), approved: false }]));

  it('WHEN approving without choosing a group THEN the approval names no group', async () => {
    const api = pendingApi();
    await openDashboard(api);
    goTo('Agents');

    fireEvent.click(await screen.findByRole('button', { name: 'Approve' }));

    await waitFor(() => expect(api.count('POST /api/agents/new-host/approve')).toBe(1));
    expect(api.calls.find((call) => call.path === '/api/agents/new-host/approve')?.body).toEqual({ group_ids: [] });
  });

  it('WHEN the operator picks the second group THEN only that group is sent, not the first one', async () => {
    const api = pendingApi();
    await openDashboard(api);
    goTo('Agents');

    fireEvent.change(await screen.findByLabelText('Group for new-host'), { target: { value: '2' } });
    fireEvent.click(screen.getByRole('button', { name: 'Approve' }));

    await waitFor(() => expect(api.count('POST /api/agents/new-host/approve')).toBe(1));
    expect(api.calls.find((call) => call.path === '/api/agents/new-host/approve')?.body).toEqual({ group_ids: [2] });
  });
});

describe('GIVEN an agent that belongs to two groups', () => {
  const multiGroupApi = (role: UserRole) =>
    apiFor(role).on('GET /api/agents', ok([{ ...agentRecord('agent-1', 'web-01'), group_ids: [1, 2], config_revision: 'rev-1' }]));

  const openConfigure = async (api: FakeApi) => {
    await openDashboard(api);
    goTo('Agents');
    fireEvent.click(await screen.findByRole('button', { name: 'Configure' }));
    await screen.findByRole('group', { name: 'Groups' });
  };

  it('WHEN the modal opens THEN both memberships are shown as checked', async () => {
    await openConfigure(multiGroupApi('global_admin'));

    expect(screen.getByRole('checkbox', { name: 'Prod' })).toBeChecked();
    expect(screen.getByRole('checkbox', { name: 'Dev' })).toBeChecked();
  });

  it('WHEN a global admin saves unchanged THEN every membership and the opened revision are sent back', async () => {
    const api = multiGroupApi('global_admin');
    await openConfigure(api);

    fireEvent.click(screen.getByRole('button', { name: 'Save' }));

    await waitFor(() => expect(api.count('POST /api/agents/agent-1/collector_config')).toBe(1));
    const body = api.calls.find((call) => call.path === '/api/agents/agent-1/collector_config')?.body as { group_ids: number[]; config_revision: string };
    expect(body.group_ids).toEqual([1, 2]);
    expect(body.config_revision).toBe('rev-1');
  });

  it('WHEN one membership is unticked THEN only that group is removed', async () => {
    const api = multiGroupApi('global_admin');
    await openConfigure(api);

    fireEvent.click(screen.getByRole('checkbox', { name: 'Prod' }));
    fireEvent.click(screen.getByRole('button', { name: 'Save' }));

    await waitFor(() => expect(api.count('POST /api/agents/agent-1/collector_config')).toBe(1));
    const body = api.calls.find((call) => call.path === '/api/agents/agent-1/collector_config')?.body as { group_ids: number[] };
    expect(body.group_ids).toEqual([2]);
  });

  it('WHEN a group operator opens it THEN the group boxes are read-only', async () => {
    await openConfigure(multiGroupApi('group_operator'));

    expect(screen.getByRole('checkbox', { name: 'Prod' })).toBeDisabled();
  });

  it('WHEN someone else saved first and the server answers 409 THEN the modal stays open with the message', async () => {
    const api = multiGroupApi('global_admin').on('POST /api/agents/agent-1/collector_config', fail(409, 'changed by someone else'));
    await openConfigure(api);

    fireEvent.click(screen.getByRole('button', { name: 'Save' }));

    expect(await screen.findByText(/changed by someone else/)).toBeInTheDocument();
    expect(screen.getByRole('group', { name: 'Groups' })).toBeInTheDocument();
  });
});

describe('GIVEN views in several groups', () => {
  it('WHEN a group operator opens the views page THEN they can edit their own group views, not another group or a public one', async () => {
    const api = apiFor('group_operator')
      .on('GET /api/groups', ok([GROUPS[0]]))
      .on('GET /api/views', ok([view(1, 'prod-view', 1), view(2, 'dev-view', 2), view(3, 'mine', 0, 'tester')]));
    await openDashboard(api);
    goTo('Views');

    const row = async (name: string) => (await screen.findByText(name)).closest('tr') as HTMLElement;

    expect(within(await row('prod-view')).getByRole('button', { name: 'Edit' })).toBeInTheDocument();
    expect(within(await row('mine')).getByRole('button', { name: 'Edit' })).toBeInTheDocument();
    expect(within(await row('dev-view')).queryByRole('button', { name: 'Edit' })).toBeNull();
  });
});

describe('GIVEN a bookmark to a view that does not exist', () => {
  it('WHEN the page loads THEN the filter is cleared and a notice explains why', async () => {
    window.location.hash = '#view-99';
    const api = apiFor('group_viewer').on('GET /api/views', ok([view(1, 'prod-view', 1)]));
    await openDashboard(api);

    expect(await screen.findByText(/View 99 was not found/)).toBeInTheDocument();
    expect(screen.queryByText(/Filtered to view/)).toBeNull();
    expect(await screen.findByText('web-01')).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Dismiss' }));

    expect(screen.queryByText(/View 99 was not found/)).toBeNull();
    expect(window.location.hash).toBe('');
  });
});
