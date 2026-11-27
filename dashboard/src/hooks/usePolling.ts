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
 * `fetcher` may change between renders; the latest one is always used.
 */
export function usePolling<T>(fetcher: () => Promise<T>, intervalMs?: number) {
  const [state, setState] = React.useState<PollingState<T>>({ data: null, error: null, loading: true, loadedAt: null });
  const fetcherRef = React.useRef(fetcher);
  const ticketRef = React.useRef(0); // id of the newest request; older responses are stale
  const runningRef = React.useRef(0); // id of the request in flight, 0 when idle

  React.useEffect(() => {
    fetcherRef.current = fetcher;
  });

  const load = React.useCallback(async (force: boolean): Promise<void> => {
    if (runningRef.current !== 0 && !force) return;
    const ticket = ++ticketRef.current;
    runningRef.current = ticket;
    try {
      const data = await fetcherRef.current();
      if (ticket !== ticketRef.current) return;
      setState({ data, error: null, loading: false, loadedAt: new Date() });
    } catch (err) {
      if (ticket !== ticketRef.current) return;
      setState((previous) => ({ ...previous, error: errorMessage(err, 'Failed to load data'), loading: false }));
    } finally {
      if (runningRef.current === ticket) runningRef.current = 0;
    }
  }, []);

  React.useEffect(() => {
    void load(true);
    if (!intervalMs) {
      return () => {
        ticketRef.current++;
        runningRef.current = 0;
      };
    }
    const poll = () => {
      if (!document.hidden) void load(false);
    };
    const timer = window.setInterval(poll, intervalMs);
    document.addEventListener('visibilitychange', poll);
    return () => {
      window.clearInterval(timer);
      document.removeEventListener('visibilitychange', poll);
      ticketRef.current++; // drop anything still in flight
      runningRef.current = 0;
    };
  }, [intervalMs, load]);

  const refresh = React.useCallback(() => load(true), [load]);

  return { ...state, refresh };
}
