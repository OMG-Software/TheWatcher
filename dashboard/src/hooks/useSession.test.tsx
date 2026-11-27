import { act, renderHook } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import type { SessionInfo } from '../models';
import { deferred } from '../testing/fakeApi';
import { useSession } from './useSession';

vi.mock('../api', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../api')>()),
  fetchSession: vi.fn(),
  logout: vi.fn().mockResolvedValue(undefined),
}));

const api = await import('../api');
const alice: SessionInfo = { username: 'alice', role: 'global_admin' };
const bob: SessionInfo = { username: 'bob', role: 'global_viewer' };

afterEach(() => vi.clearAllMocks());

describe('GIVEN a session probe that is still running', () => {
  it('WHEN the user signs in before it settles THEN the late probe result does not replace the new session', async () => {
    const probe = deferred<SessionInfo>();
    vi.mocked(api.fetchSession).mockReturnValue(probe.promise);
    const { result } = renderHook(() => useSession());

    act(() => result.current.signIn(bob));
    await act(async () => probe.resolve(alice));

    expect(result.current.session).toEqual(bob);
  });

  it('WHEN the user signs out before it settles THEN the late probe result does not sign them back in', async () => {
    const probe = deferred<SessionInfo>();
    vi.mocked(api.fetchSession).mockReturnValue(probe.promise);
    const { result } = renderHook(() => useSession());

    act(() => result.current.signOut());
    await act(async () => probe.resolve(alice));

    expect(result.current.session).toBeNull();
  });

  it('WHEN nothing else happened THEN the probe restores the session', async () => {
    vi.mocked(api.fetchSession).mockResolvedValue(alice);

    const { result } = renderHook(() => useSession());
    await act(async () => undefined);

    expect(result.current.session).toEqual(alice);
  });
});

describe('GIVEN a signed-in session', () => {
  it('WHEN an API call reports 401 THEN the session ends as expired, and a deliberate logout is not reported as expiry', async () => {
    vi.mocked(api.fetchSession).mockResolvedValue(alice);
    const { result } = renderHook(() => useSession());
    await act(async () => undefined);

    act(() => result.current.signOut());

    expect(result.current.expired).toBe(false);
    expect(result.current.session).toBeNull();
  });
});
