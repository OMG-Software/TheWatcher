import { afterEach, describe, expect, it, vi } from 'vitest';
import { ApiError, deleteAgent, fetchAgents, fetchSession, loadDashboardData, login, onUnauthorized, updateSettings } from './api';

function failure(status: number, statusText: string, body?: unknown) {
  return {
    ok: false,
    status,
    statusText,
    json: async () => {
      if (body === undefined) throw new SyntaxError('no body');
      return body;
    },
  };
}

describe('GIVEN the API client', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('WHEN the server rejects a request with an error body THEN the ApiError carries the status and the server message', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(failure(403, 'Forbidden', { error: 'permission denied' })));

    const error = await deleteAgent('a1').catch((err: unknown) => err);

    expect(error).toBeInstanceOf(ApiError);
    expect((error as ApiError).status).toBe(403);
    expect((error as ApiError).message).toBe('permission denied');
  });

  it('WHEN the server sends no usable body THEN the message falls back to the status line', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(failure(502, 'Bad Gateway')));

    const error = await fetchAgents().catch((err: unknown) => err);

    expect((error as ApiError).status).toBe(502);
    expect((error as ApiError).message).toBe('502 Bad Gateway');
  });

  it('WHEN a PUT fails THEN the server message is kept too', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(failure(400, 'Bad Request', { error: 'offline_after_seconds out of range' })));

    await expect(updateSettings({ offline_after_seconds: 1 })).rejects.toThrow('offline_after_seconds out of range');
  });
});

describe('GIVEN a signed-in session', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('WHEN any API call returns 401 THEN the unauthorized listeners are told once per call', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(failure(401, 'Unauthorized', { error: 'authentication required' })));
    const listener = vi.fn();
    const stop = onUnauthorized(listener);

    await expect(fetchAgents()).rejects.toBeInstanceOf(ApiError);
    stop();
    await expect(fetchAgents()).rejects.toBeInstanceOf(ApiError);

    expect(listener).toHaveBeenCalledTimes(1);
  });

  it('WHEN the login or session probe returns 401 THEN listeners are not told, because that is the normal signed-out state', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(failure(401, 'Unauthorized', { error: 'invalid credentials' })));
    const listener = vi.fn();
    const stop = onUnauthorized(listener);

    await expect(login('admin', 'wrong')).rejects.toThrow('invalid credentials');
    await expect(fetchSession()).rejects.toBeInstanceOf(ApiError);
    stop();

    expect(listener).not.toHaveBeenCalled();
  });
});

describe('GIVEN loadDashboardData with failing sections', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  const respond = (failing: Record<string, ReturnType<typeof failure>>) =>
    vi.fn().mockImplementation((url: string) =>
      Promise.resolve(failing[url] ?? { ok: true, json: async () => [] }),
    );

  it('WHEN a section fails with a server error THEN it is reported in errors and not passed off as empty data', async () => {
    vi.stubGlobal('fetch', respond({ '/api/alerts/unacknowledged': failure(500, 'Internal Server Error', { error: 'database is locked' }) }));

    const data = await loadDashboardData();

    expect(data.alerts).toEqual([]);
    expect(data.errors).toEqual({ alerts: 'database is locked' });
  });

  it('WHEN a section is forbidden for the role THEN it is empty and not an error', async () => {
    vi.stubGlobal('fetch', respond({ '/api/users': failure(403, 'Forbidden', { error: 'permission denied' }) }));

    const data = await loadDashboardData();

    expect(data.users).toEqual([]);
    expect(data.errors).toEqual({});
  });

  it('WHEN the agents list fails THEN the whole load rejects with the ApiError', async () => {
    vi.stubGlobal('fetch', respond({ '/api/agents': failure(500, 'Internal Server Error', { error: 'store unavailable' }) }));

    await expect(loadDashboardData()).rejects.toMatchObject({ status: 500, message: 'store unavailable' });
  });
});
