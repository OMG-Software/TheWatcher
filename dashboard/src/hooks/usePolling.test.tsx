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

  it('WHEN refresh is called while a request is in flight THEN it waits for it, and the follow-up result is the final one', async () => {
    const older = deferred<string>();
    const fetcher = vi.fn().mockReturnValueOnce(older.promise).mockResolvedValueOnce('fresh');
    const { result } = renderHook(() => usePolling(fetcher, { intervalMs: 1000 }));

    let refreshing!: Promise<unknown>;
    act(() => {
      refreshing = result.current.refresh();
    });
    expect(fetcher).toHaveBeenCalledTimes(1); // the older request is not duplicated
    await act(async () => {
      older.resolve('older');
      await refreshing;
    });

    expect(fetcher).toHaveBeenCalledTimes(2);
    expect(result.current.data).toBe('fresh');
  });

  it('WHEN refresh is called three times during one pending request THEN exactly one follow-up runs and all three calls get its result', async () => {
    const first = deferred<string>();
    const fetcher = vi.fn().mockReturnValueOnce(first.promise).mockRejectedValueOnce(new Error('follow-up failed'));
    const { result } = renderHook(() => usePolling(fetcher));

    const results: Promise<string | null>[] = [];
    act(() => {
      results.push(result.current.refresh(), result.current.refresh(), result.current.refresh());
    });
    await act(async () => {
      first.resolve('first');
      await Promise.all(results);
    });

    expect(fetcher).toHaveBeenCalledTimes(2);
    expect(await Promise.all(results)).toEqual(['follow-up failed', 'follow-up failed', 'follow-up failed']);
  });

  it('WHEN a poll tick arrives while a request is in flight or a follow-up is queued THEN it makes no call', async () => {
    const first = deferred<string>();
    const fetcher = vi.fn().mockReturnValueOnce(first.promise).mockResolvedValue('later');
    const { result } = renderHook(() => usePolling(fetcher, { intervalMs: 1000 }));
    act(() => {
      void result.current.refresh();
    });

    await advance(3000);

    expect(fetcher).toHaveBeenCalledTimes(1);
  });

  it('WHEN the hook is unmounted with a follow-up queued THEN the follow-up never runs and refresh still resolves', async () => {
    const first = deferred<string>();
    const fetcher = vi.fn().mockReturnValueOnce(first.promise).mockResolvedValue('later');
    const { result, unmount } = renderHook(() => usePolling(fetcher));
    let refreshing!: Promise<string | null>;
    act(() => {
      refreshing = result.current.refresh();
    });

    unmount();
    first.resolve('first');

    expect(await refreshing).toBeNull();
    expect(fetcher).toHaveBeenCalledTimes(1);
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

describe('GIVEN a polling hook whose fetcher takes an abort signal', () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  /** A fetcher that stays pending until its signal aborts, like a real fetch. */
  function abortableFetcher() {
    const signals: AbortSignal[] = [];
    const fetcher = vi.fn((signal: AbortSignal) => {
      signals.push(signal);
      return new Promise<string>((_resolve, reject) => {
        signal.addEventListener('abort', () => reject(new DOMException('aborted', 'AbortError')));
      });
    });
    return { fetcher, signals };
  }

  it('WHEN a request starts THEN the fetcher gets a live signal', async () => {
    const { fetcher, signals } = abortableFetcher();

    renderHook(() => usePolling(fetcher));
    await advance(0);

    expect(signals).toHaveLength(1);
    expect(signals[0].aborted).toBe(false);
  });

  it('WHEN the hook unmounts mid-request THEN the request is aborted', async () => {
    const { fetcher, signals } = abortableFetcher();
    const { unmount } = renderHook(() => usePolling(fetcher));
    await advance(0);

    unmount();

    expect(signals[0].aborted).toBe(true);
  });

  it('WHEN polling is disabled mid-request THEN the request is aborted and no error is shown', async () => {
    const { fetcher, signals } = abortableFetcher();
    const { result, rerender } = renderHook(({ enabled }) => usePolling(fetcher, { enabled }), { initialProps: { enabled: true } });
    await advance(0);

    rerender({ enabled: false });
    await advance(0);

    expect(signals[0].aborted).toBe(true);
    expect(result.current.error).toBeNull();
  });

  it('WHEN a request is aborted by the hook THEN its refresh promise resolves null and nothing is reported', async () => {
    const { fetcher } = abortableFetcher();
    const { result, unmount } = renderHook(() => usePolling(fetcher));
    let refreshing!: Promise<string | null>;
    act(() => {
      refreshing = result.current.refresh();
    });

    unmount();

    expect(await refreshing).toBeNull();
  });

  it('WHEN the fetcher rejects with an abort the hook did not cause THEN it is not shown as an error', async () => {
    const fetcher = vi.fn().mockRejectedValue(new DOMException('aborted', 'AbortError'));

    const { result } = renderHook(() => usePolling(fetcher));
    await advance(0);

    expect(result.current.error).toBeNull();
  });

  it('WHEN a fetcher ignores the signal and resolves after unmount THEN the late result is still ignored', async () => {
    const late = deferred<string>();
    const fetcher = vi.fn(() => late.promise);
    const { result, unmount } = renderHook(() => usePolling(fetcher));
    await advance(0);

    unmount();
    late.resolve('late');
    await advance(0);

    expect(result.current.data).toBeNull();
  });

  it('WHEN reload is called mid-request THEN the old request is aborted, its result is ignored and the fresh one is shown', async () => {
    const first = deferred<string>();
    const signals: AbortSignal[] = [];
    const fetcher = vi.fn((signal: AbortSignal) => {
      signals.push(signal);
      return signals.length === 1 ? first.promise : Promise.resolve('fresh');
    });
    const { result } = renderHook(() => usePolling(fetcher));
    await advance(0);

    let reloading!: Promise<string | null>;
    act(() => {
      reloading = result.current.reload();
    });
    await act(async () => {
      first.resolve('stale');
      await reloading;
    });

    expect(signals[0].aborted).toBe(true);
    expect(fetcher).toHaveBeenCalledTimes(2);
    expect(result.current.data).toBe('fresh');
  });

  it('WHEN reload is called with nothing in flight THEN it simply loads', async () => {
    const fetcher = vi.fn().mockResolvedValueOnce('one').mockResolvedValueOnce('two');
    const { result } = renderHook(() => usePolling(fetcher));
    await advance(0);

    await act(async () => {
      expect(await result.current.reload()).toBeNull();
    });

    expect(result.current.data).toBe('two');
  });
});
