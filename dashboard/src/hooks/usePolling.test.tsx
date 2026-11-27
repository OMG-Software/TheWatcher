import { act, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { usePolling } from './usePolling';

/** A promise the test settles by hand, to control response ordering. */
function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason: unknown) => void;
  const promise = new Promise<T>((res, rej) => {
    resolve = res;
    reject = rej;
  });
  return { promise, resolve, reject };
}

function setTabHidden(hidden: boolean) {
  Object.defineProperty(document, 'hidden', { configurable: true, value: hidden });
  document.dispatchEvent(new Event('visibilitychange'));
}

async function advance(ms: number) {
  await act(async () => {
    await vi.advanceTimersByTimeAsync(ms);
  });
}

describe('GIVEN a polling hook', () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });
  afterEach(() => {
    setTabHidden(false);
    vi.useRealTimers();
  });

  it('WHEN mounted THEN it loads at once and again every interval', async () => {
    const fetcher = vi.fn().mockResolvedValue('data');

    const { result } = renderHook(() => usePolling(fetcher, { intervalMs: 1000 }));
    await advance(0);
    expect(result.current.data).toBe('data');
    expect(result.current.loading).toBe(false);
    await advance(2000);

    expect(fetcher).toHaveBeenCalledTimes(3);
  });

  it('WHEN a request is still in flight THEN later ticks do not start another request', async () => {
    const slow = deferred<string>();
    const fetcher = vi.fn().mockReturnValueOnce(slow.promise).mockResolvedValue('next');

    renderHook(() => usePolling(fetcher, { intervalMs: 1000 }));
    await advance(3500);
    expect(fetcher).toHaveBeenCalledTimes(1);

    slow.resolve('first');
    await advance(1000);

    expect(fetcher).toHaveBeenCalledTimes(2);
  });

  it('WHEN the tab is hidden THEN polling pauses, and it refreshes at once when the tab is visible again', async () => {
    const fetcher = vi.fn().mockResolvedValue('data');
    renderHook(() => usePolling(fetcher, { intervalMs: 1000 }));
    await advance(0);
    expect(fetcher).toHaveBeenCalledTimes(1);

    setTabHidden(true);
    await advance(5000);
    expect(fetcher).toHaveBeenCalledTimes(1);

    setTabHidden(false);
    await advance(0);
    expect(fetcher).toHaveBeenCalledTimes(2);
  });

  it('WHEN a manual refresh overtakes an older request THEN the older response is ignored', async () => {
    const older = deferred<string>();
    const newer = deferred<string>();
    const fetcher = vi.fn().mockReturnValueOnce(older.promise).mockReturnValueOnce(newer.promise);
    const { result } = renderHook(() => usePolling(fetcher, { intervalMs: 1000 }));

    let refreshing!: Promise<unknown>;
    act(() => {
      refreshing = result.current.refresh();
    });
    await act(async () => {
      newer.resolve('fresh');
      await refreshing;
    });
    await act(async () => {
      older.resolve('stale');
      await Promise.resolve();
    });

    expect(result.current.data).toBe('fresh');
  });

  it('WHEN a refresh fails after a good load THEN the data is kept and the error is reported, then cleared by the next success', async () => {
    const fetcher = vi
      .fn()
      .mockResolvedValueOnce('good')
      .mockRejectedValueOnce(new Error('boom'))
      .mockResolvedValue('recovered');
    const { result } = renderHook(() => usePolling(fetcher, { intervalMs: 1000 }));
    await advance(0);

    await advance(1000);
    expect(result.current.data).toBe('good');
    expect(result.current.error).toBe('boom');

    await advance(1000);
    expect(result.current.data).toBe('recovered');
    expect(result.current.error).toBeNull();
  });

  it('WHEN the first load fails THEN loading ends with an error and no data', async () => {
    const fetcher = vi.fn().mockRejectedValue(new Error('server down'));

    const { result } = renderHook(() => usePolling(fetcher));
    await advance(0);

    expect(result.current).toMatchObject({ data: null, error: 'server down', loading: false });
  });

  it('WHEN disabled THEN it neither loads nor polls', async () => {
    const fetcher = vi.fn().mockResolvedValue('x');

    renderHook(() => usePolling(fetcher, { intervalMs: 1000, enabled: false }));
    await advance(10_000);

    expect(fetcher).not.toHaveBeenCalled();
  });

  it('WHEN no interval is given THEN it loads once and never polls', async () => {
    const fetcher = vi.fn().mockResolvedValue('once');

    renderHook(() => usePolling(fetcher));
    await advance(60_000);

    expect(fetcher).toHaveBeenCalledTimes(1);
  });
});
