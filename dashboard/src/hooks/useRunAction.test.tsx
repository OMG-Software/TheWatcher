import { act, renderHook } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { deferred } from '../testing/fakeApi';
import { usePolling } from './usePolling';
import { useRunAction } from './useRunAction';

describe('GIVEN a mutation that succeeds', () => {
  it('WHEN the refresh afterwards fails THEN the action still reports success and the refresh failure is a page-level banner', async () => {
    const refresh = vi.fn().mockRejectedValue(new Error('store unavailable'));
    const action = vi.fn().mockResolvedValue(undefined);
    const { result } = renderHook(() => useRunAction(refresh));

    let saved: boolean | undefined;
    await act(async () => {
      saved = await result.current.runAction('silence:create', action, { inline: true });
    });

    expect(saved).toBe(true);
    expect(action).toHaveBeenCalledTimes(1);
    expect(result.current.failure?.inline).toBe(false);
    expect(result.current.failure?.message).toMatch(/could not be refreshed.*store unavailable/);
  });
});

describe('GIVEN a mutation that fails', () => {
  it('WHEN it runs THEN the action reports failure, skips the refresh and keeps the failure under its key', async () => {
    const refresh = vi.fn().mockResolvedValue(undefined);
    const { result } = renderHook(() => useRunAction(refresh));

    let saved: boolean | undefined;
    await act(async () => {
      saved = await result.current.runAction('x', () => Promise.reject(new Error('nope')), { inline: true });
    });

    expect(saved).toBe(false);
    expect(refresh).not.toHaveBeenCalled();
    expect(result.current.failure).toEqual({ key: 'x', message: 'nope', inline: true });
  });
});

describe('GIVEN a failure shown by a dialog', () => {
  const failed = async () => {
    const hook = renderHook(() => useRunAction(vi.fn().mockResolvedValue(undefined)));
    await act(async () => {
      await hook.result.current.runAction('alert:1:ack', () => Promise.reject(new Error('nope')), { inline: true });
    });
    return hook;
  };

  it('WHEN that dialog clears its own key THEN the failure is gone', async () => {
    const { result } = await failed();

    act(() => result.current.clearFailure('alert:1:ack'));

    expect(result.current.failure).toBeNull();
  });

  it('WHEN a different key is cleared THEN the failure stays', async () => {
    const { result } = await failed();

    act(() => result.current.clearFailure('alert:2:ack'));

    expect(result.current.failure?.key).toBe('alert:1:ack');
  });
});

describe('GIVEN the real polling refresh behind runAction', () => {
  const setup = (fetcher: () => Promise<string>) =>
    renderHook(() => {
      const polling = usePolling(fetcher);
      return { polling, run: useRunAction(polling.refresh) };
    });

  it('WHEN the mutation succeeds but the reload fails THEN the action succeeds and the page banner says the refresh failed', async () => {
    const fetcher = vi.fn().mockResolvedValueOnce('first').mockRejectedValue(new Error('store unavailable'));
    const { result } = setup(fetcher);
    await act(async () => undefined);

    let saved: boolean | undefined;
    await act(async () => {
      saved = await result.current.run.runAction('silence:create', async () => undefined, { inline: true });
    });

    expect(saved).toBe(true);
    expect(result.current.run.failure?.inline).toBe(false);
    expect(result.current.run.failure?.message).toMatch(/could not be refreshed.*store unavailable/);
  });

  it('WHEN the mutation succeeds and the reload succeeds THEN no failure is reported', async () => {
    const { result } = setup(vi.fn().mockResolvedValue('data'));
    await act(async () => undefined);

    await act(async () => {
      await result.current.run.runAction('x', async () => undefined);
    });

    expect(result.current.run.failure).toBeNull();
  });

  it('WHEN a manual refresh is queued behind the action refresh THEN the action reports its own refresh result and the data ends fresh', async () => {
    let calls = 0;
    const actionRefresh = deferred<string>();
    const fetcher = vi.fn(() => {
      calls++;
      if (calls === 1) return Promise.resolve('first');
      if (calls === 2) return actionRefresh.promise; // the action's refresh
      return Promise.resolve('newer'); // the manual refresh, queued behind it
    });
    const { result } = setup(fetcher);
    await act(async () => undefined);

    let running!: Promise<boolean>;
    act(() => {
      running = result.current.run.runAction('x', async () => undefined);
    });
    await act(async () => undefined);
    let manual!: Promise<string | null>;
    act(() => {
      manual = result.current.polling.refresh();
    });
    await act(async () => {
      actionRefresh.resolve('after-action');
      await running;
      await manual;
    });

    expect(fetcher).toHaveBeenCalledTimes(3);
    expect(result.current.run.failure).toBeNull();
    expect(result.current.polling.data).toBe('newer');
  });
});

describe('GIVEN an action whose refresh is still running', () => {
  it('WHEN the mutation has finished THEN the action stays busy until the refresh settles, then reports success', async () => {
    const refreshing = deferred<string | null>();
    const { result } = renderHook(() => useRunAction(() => refreshing.promise));

    let running!: Promise<boolean>;
    act(() => {
      running = result.current.runAction('silence:create', async () => undefined);
    });
    await act(async () => undefined);

    expect(result.current.busyAction).toBe('silence:create');
    await act(async () => {
      refreshing.resolve(null);
      expect(await running).toBe(true);
    });
    expect(result.current.busyAction).toBeNull();
  });
});
