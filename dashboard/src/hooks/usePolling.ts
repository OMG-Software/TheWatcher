import React from 'react';
import { errorMessage } from '../api';

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
 * - Disabling or unmounting drops whatever is in flight (its result is ignored) and the queued follow-up.
 * - The poll pauses while the tab is hidden and runs at once when it is shown again.
 *
 * `enabled: false` neither loads nor polls (data from an earlier enabled period is kept).
 * `merge` combines an accepted result with the previous data (for example to keep the last
 * good part of a partial failure); it never sees a stale response.
 * `refresh()` resolves with the error message of a load that started after the call, or null
 * on success (and null when the hook was disabled or unmounted first). It never rejects.
 * `fetcher` may change between renders; the latest one is always used.
 */
export function usePolling<T>(fetcher: () => Promise<T>, { intervalMs, enabled = true, merge }: { intervalMs?: number; enabled?: boolean; merge?: (next: T, previous: T | null) => T } = {}) {
  const [state, setState] = React.useState<PollingState<T>>({ data: null, error: null, loading: true, loadedAt: null });
  const fetcherRef = React.useRef(fetcher);
  const mergeRef = React.useRef(merge);
  const generationRef = React.useRef(0); // bumped by invalidate(); a load from an older generation is ignored
  const inFlightRef = React.useRef<Promise<string | null> | null>(null);
  const queuedRef = React.useRef<{ promise: Promise<string | null>; start: () => void; cancel: () => void } | null>(null);

  React.useEffect(() => {
    fetcherRef.current = fetcher;
    mergeRef.current = merge;
  });

  // Runs one request and applies its result unless the hook was invalidated meanwhile.
  const run = React.useCallback(async (): Promise<string | null> => {
    const generation = generationRef.current;
    try {
      const data = await fetcherRef.current();
      if (generation !== generationRef.current) return null;
      const merger = mergeRef.current;
      setState((previous) => ({ data: merger ? merger(data, previous.data) : data, error: null, loading: false, loadedAt: new Date() }));
      return null;
    } catch (err) {
      if (generation !== generationRef.current) return null;
      const message = errorMessage(err, 'Failed to load data');
      setState((previous) => ({ ...previous, error: message, loading: false }));
      return message;
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

  // Drops whatever is in flight (its result is ignored) and the queued follow-up.
  const invalidate = React.useCallback(() => {
    generationRef.current++;
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

  return { ...state, refresh };
}
