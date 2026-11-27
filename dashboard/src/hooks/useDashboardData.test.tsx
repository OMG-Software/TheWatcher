import { act, renderHook } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { DashboardData } from '../api';
import type { GroupRecord } from '../models';
import { deferred } from '../testing/fakeApi';
import { useDashboardData } from './useDashboardData';

vi.mock('../api', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api')>()),
  loadDashboardData: vi.fn(),
}));

const api = await import('../api');

const group = (id: number): GroupRecord => ({ group_id: id, name: `group-${id}` }) as GroupRecord;
const snapshot = (groups: GroupRecord[], groupsError?: string): DashboardData =>
  ({
    agents: [], metrics: [], pending: [], groups, alerts: [], allAlerts: [], users: [], maintenanceWindows: [], silences: [], views: [],
    errors: groupsError ? { groups: groupsError } : {},
  }) as DashboardData;

afterEach(() => vi.clearAllMocks());

describe('GIVEN two overlapping dashboard loads', () => {
  it('WHEN the older load settles last THEN a later section failure keeps the newer load\'s data, not the stale one', async () => {
    const older = deferred<DashboardData>();
    const newer = deferred<DashboardData>();
    vi.mocked(api.loadDashboardData)
      .mockReturnValueOnce(older.promise) // the initial load
      .mockReturnValueOnce(newer.promise) // a manual refresh overtakes it
      .mockResolvedValueOnce(snapshot([], 'database is locked')); // the next load fails its groups section
    const { result } = renderHook(() => useDashboardData());

    let refreshing!: Promise<unknown>;
    act(() => {
      refreshing = result.current.refresh();
    });
    await act(async () => newer.resolve(snapshot([group(2)])));
    await act(async () => older.resolve(snapshot([group(1)])));
    await refreshing;
    await act(async () => result.current.refresh());

    expect(result.current.groups.map((g) => g.group_id)).toEqual([2]);
    expect(result.current.sectionErrors.groups).toBe('database is locked');
  });
});
