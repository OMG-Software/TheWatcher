import React from 'react';
import { errorMessage } from '../api';

function isAbortError(err: unknown): boolean {
  return typeof err === 'object' && err !== null && 'name' in err && err.name === 'AbortError';
}

export interface PollingState<T> {
  /** Last good result; kept when a later refresh fails so the page does not blank out. */
  data: T | null;
  /** Message of the most recent failed load; null after a success. */
  error: string | null;
  /** True until the first load settles, either way. */
  loading: boolean;
  loadedAt: Date | null;
}

/**
 * Loads data once and, when `intervalMs` is given, again on that interval.
 *
 * - At most one request is in flight. A poll tick is skipped while one runs (or is queued).
 * - `refresh()` while a request is in flight queues exactly one follow-up load, started when the
 *   current one settles (the running one may predate the change that prompted the refresh).
 *   Further `refresh()` calls share that follow-up.
 * - `reload()` is `refresh()` for when the running request is known to be out of date (for example
 *   after the user changed something it would not show): it aborts the running request, ignores its
 *   result and starts a new one at once.
 * - Disabling or unmounting aborts whatever is in flight (its result is ignored even if the fetcher
 *   does not honour the signal) and drops the queued follow-up.
 * - The poll pauses while the tab is hidden and runs at once when it is shown again.
 *
 * `enabled: false` neither loads nor polls (data from an earlier enabled period is kept).
 * `merge` combines an accepted result with the previous data (for example to keep the last
 * good part of a partial failure); it never sees a stale response.
 * `refresh()` resolves with the error message of a load that started after the call, or null
 * on success (and null when the hook was disabled or unmounted first). It never rejects.
 * `fetcher` receives an `AbortSignal` for its request and may pass it to `fetch`; a fetcher that
 * ignores the signal still works. An abort is never reported as an error.
 * `fetcher` may change between renders; the latest one is always used.
 */
export function usePolling<T>(fetcher: (signal: AbortSignal) => Promise<T>, { intervalMs, enabled = true, merge }: { intervalMs?: number; enabled?: boolean; merge?: (next: T, previous: T | null) => T } = {}) {
  const [state, setState] = React.useState<PollingState<T>>({ data: null, error: null, loading: true, loadedAt: null });
  const fetcherRef = React.useRef(fetcher);
  const mergeRef = React.useRef(merge);
  const generationRef = React.useRef(0); // bumped by invalidate(); a load from an older generation is ignored
  const controllerRef = React.useRef<AbortController | null>(null); // aborts the running request
  const inFlightRef = React.useRef<Promise<string | null> | null>(null);
  const queuedRef = React.useRef<{ promise: Promise<string | null>; start: () => void; cancel: () => void } | null>(null);

  React.useEffect(() => {
    fetcherRef.current = fetcher;
    mergeRef.current = merge;
  });

  // Runs one request and applies its result unless the hook was invalidated meanwhile.
  const run = React.useCallback(async (): Promise<string | null> => {
    const generation = generationRef.current;
    const controller = new AbortController();
    controllerRef.current = controller;
    try {
      const data = await fetcherRef.current(controller.signal);
      if (generation !== generationRef.current) return null;
      const merger = mergeRef.current;
      setState((previous) => ({ data: merger ? merger(data, previous.data) : data, error: null, loading: false, loadedAt: new Date() }));
      return null;
    } catch (err) {
      // An abort is a cancellation, not a failure; so is anything that settles after invalidation.
      if (generation !== generationRef.current || isAbortError(err)) return null;
      const message = errorMessage(err, 'Failed to load data');
      setState((previous) => ({ ...previous, error: message, loading: false }));
      return message;
    } finally {
      if (controllerRef.current === controller) controllerRef.current = null;
    }
  }, []);

  const startLoad = React.useCallback((): Promise<string | null> => {
    const promise = run();
    inFlightRef.current = promise;
    void promise.then(() => {
      if (inFlightRef.current !== promise) return; // invalidated meanwhile
      inFlightRef.current = null;
      const queued = queuedRef.current;
      queuedRef.current = null;
      queued?.start();
    });
    return promise;
  }, [run]);

  const load = React.useCallback(
    (force: boolean): Promise<string | null> => {
      if (inFlightRef.current === null) return startLoad();
      if (!force) return Promise.resolve(null);
      if (queuedRef.current) return queuedRef.current.promise;
      let start!: () => void;
      let cancel!: () => void;
      const promise = new Promise<string | null>((resolve) => {
        start = () => void startLoad().then(resolve);
        cancel = () => resolve(null);
      });
      queuedRef.current = { promise, start, cancel };
      return promise;
    },
    [startLoad],
  );

  // Aborts whatever is in flight (its result is ignored) and drops the queued follow-up.
  const invalidate = React.useCallback(() => {
    generationRef.current++;
    controllerRef.current?.abort();
    controllerRef.current = null;
    inFlightRef.current = null;
    queuedRef.current?.cancel();
    queuedRef.current = null;
  }, []);

  React.useEffect(() => {
    if (!enabled) return;
    void load(true);
    if (!intervalMs) return invalidate;
    const poll = () => {
      if (!document.hidden) void load(false);
    };
    const timer = window.setInterval(poll, intervalMs);
    document.addEventListener('visibilitychange', poll);
    return () => {
      window.clearInterval(timer);
      document.removeEventListener('visibilitychange', poll);
      invalidate();
    };
  }, [enabled, intervalMs, invalidate, load]);

  const refresh = React.useCallback(() => load(true), [load]);

  const reload = React.useCallback((): Promise<string | null> => {
    invalidate();
    return startLoad();
  }, [invalidate, startLoad]);

  return { ...state, refresh, reload };
}
