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

    await act(async () => result.current.signOut());
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

    await act(async () => result.current.signOut());

    expect(result.current.expired).toBe(false);
    expect(result.current.session).toBeNull();
  });
});

async function signedInHook() {
  vi.mocked(api.fetchSession).mockResolvedValue(alice);
  const hook = renderHook(() => useSession());
  await act(async () => undefined);
  return hook;
}

describe('GIVEN a signed-in user who signs out', () => {
  it('WHEN the server has not answered yet THEN the user stays signed in', async () => {
    const reply = deferred<void>();
    vi.mocked(api.logout).mockReturnValueOnce(reply.promise);
    const { result } = await signedInHook();

    let signingOut!: Promise<void>;
    act(() => {
      signingOut = result.current.signOut();
    });

    expect(result.current.session).toEqual(alice);
    expect(result.current.signingOut).toBe(true);
    await act(async () => {
      reply.resolve();
      await signingOut;
    });
    expect(result.current.session).toBeNull();
    expect(result.current.signingOut).toBe(false);
  });

  it('WHEN the server answers 401 THEN the session was already gone, so the user is signed out without an error', async () => {
    vi.mocked(api.logout).mockRejectedValueOnce(new api.ApiError(401, 'authentication required'));
    const { result } = await signedInHook();

    await act(async () => result.current.signOut());

    expect(result.current.session).toBeNull();
    expect(result.current.signOutError).toBeNull();
    expect(result.current.expired).toBe(false);
  });

  it('WHEN the request fails any other way THEN the user stays signed in and is told the session is still active', async () => {
    vi.mocked(api.logout).mockRejectedValueOnce(new api.ApiError(500, 'store unavailable'));
    const { result } = await signedInHook();

    await act(async () => result.current.signOut());

    expect(result.current.session).toEqual(alice);
    expect(result.current.signOutError).toMatch(/sign.?out failed/i);
    expect(result.current.signOutError).toMatch(/store unavailable/);
    expect(result.current.signOutError).toMatch(/still active/i);
    expect(result.current.signingOut).toBe(false);
  });

  it('WHEN a failed sign-out is tried again and succeeds THEN the user is signed out and the error is gone', async () => {
    vi.mocked(api.logout).mockRejectedValueOnce(new TypeError('network down')).mockResolvedValueOnce(undefined);
    const { result } = await signedInHook();
    await act(async () => result.current.signOut());
    expect(result.current.signOutError).not.toBeNull();

    await act(async () => result.current.signOut());

    expect(result.current.session).toBeNull();
    expect(result.current.signOutError).toBeNull();
  });

  it('WHEN the failure is dismissed THEN the error clears and the user stays signed in', async () => {
    vi.mocked(api.logout).mockRejectedValueOnce(new Error('offline'));
    const { result } = await signedInHook();
    await act(async () => result.current.signOut());

    act(() => result.current.dismissSignOutError());

    expect(result.current.signOutError).toBeNull();
    expect(result.current.session).toEqual(alice);
  });

  it('WHEN sign-out is requested twice at once THEN only one request is sent', async () => {
    const reply = deferred<void>();
    vi.mocked(api.logout).mockReturnValueOnce(reply.promise);
    const { result } = await signedInHook();

    act(() => {
      void result.current.signOut();
      void result.current.signOut();
    });
    await act(async () => reply.resolve());

    expect(api.logout).toHaveBeenCalledTimes(1);
  });
});

describe('GIVEN a session that ends while sign-out is running', () => {
  it('WHEN a 401 from another call expires it first THEN the late sign-out failure does not bring an error back', async () => {
    const reply = deferred<void>();
    vi.mocked(api.logout).mockReturnValueOnce(reply.promise);
    const { result } = await signedInHook();
    act(() => void result.current.signOut());

    // Another request reports the session as expired.
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('{"error":"authentication required"}', { status: 401 })));
    await act(async () => { await api.fetchAgents().catch(() => undefined); });
    vi.unstubAllGlobals();
    await act(async () => reply.reject(new Error('late')));

    expect(result.current.session).toBeNull();
    expect(result.current.signOutError).toBeNull();
  });
});
