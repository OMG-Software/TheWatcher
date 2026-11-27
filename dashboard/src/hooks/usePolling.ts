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
 * - In-flight guard: a tick is skipped while the previous request is still running.
 * - Stale responses: a manual `refresh()` supersedes any older request, whose
 *   result is then dropped; so does unmounting.
 * - The poll pauses while the tab is hidden and runs at once when it is shown again.
 *
 * `enabled: false` neither loads nor polls (data from an earlier enabled period is kept).
 * `merge` combines an accepted result with the previous data (for example to keep the last
 * good part of a partial failure); it never sees a stale response.
 * `refresh()` resolves to the load error message, or null when it succeeded or was overtaken by
 * a newer request (an overtaken load is not a failure). It never rejects.
 * `fetcher` may change between renders; the latest one is always used.
 */
export function usePolling<T>(fetcher: () => Promise<T>, { intervalMs, enabled = true, merge }: { intervalMs?: number; enabled?: boolean; merge?: (next: T, previous: T | null) => T } = {}) {
  const [state, setState] = React.useState<PollingState<T>>({ data: null, error: null, loading: true, loadedAt: null });
  const fetcherRef = React.useRef(fetcher);
  const mergeRef = React.useRef(merge);
  const ticketRef = React.useRef(0); // id of the newest request; older responses are stale
  const runningRef = React.useRef(0); // id of the request in flight, 0 when idle

  React.useEffect(() => {
    fetcherRef.current = fetcher;
    mergeRef.current = merge;
  });

  const load = React.useCallback(async (force: boolean): Promise<string | null> => {
    if (runningRef.current !== 0 && !force) return null;
    const ticket = ++ticketRef.current;
    runningRef.current = ticket;
    try {
      const data = await fetcherRef.current();
      if (ticket !== ticketRef.current) return null;
      const merger = mergeRef.current;
      setState((previous) => ({ data: merger ? merger(data, previous.data) : data, error: null, loading: false, loadedAt: new Date() }));
      return null;
    } catch (err) {
      if (ticket !== ticketRef.current) return null; // overtaken: the newer request decides
      const message = errorMessage(err, 'Failed to load data');
      setState((previous) => ({ ...previous, error: message, loading: false }));
      return message;
    } finally {
      if (runningRef.current === ticket) runningRef.current = 0;
    }
  }, []);

  // Drops whatever is in flight: its response will be treated as stale.
  const invalidate = React.useCallback(() => {
    ticketRef.current++;
    runningRef.current = 0;
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
