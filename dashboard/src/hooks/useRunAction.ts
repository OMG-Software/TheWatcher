import React from 'react';
import { errorMessage } from '../api';

/** The latest failed action: which one, the server's message, and whether a form shows it inline. */
export interface ActionFailure {
  key: string;
  message: string;
  inline: boolean;
}

/**
 * Runs a mutation, then refreshes; resolves true on success, false on failure.
 * Pass `{ inline: true }` from a form or modal that shows the failure itself
 * with `<ActionError actionKey=...>`; other failures go to the page-level banner.
 * Callers must only clear or close their form when this resolves true.
 */
export type RunAction = (key: string, action: () => Promise<void>, options?: { inline?: boolean }) => Promise<boolean>;

/** The latest failure, plus a way for a form or dialog to drop its own failure when it closes. */
export interface ActionFailureState {
  failure: ActionFailure | null;
  /** Clears the failure; with a key, only if it belongs to that action. */
  clear: (key?: string) => void;
}

/** Provides the latest failure so `<ActionError>` can render it next to the form that caused it. */
export const ActionFailureContext = React.createContext<ActionFailureState>({ failure: null, clear: () => undefined });

/** `refresh` may resolve to a load error message (as `usePolling`'s does) or reject; either means the reload failed. */
export function useRunAction(refresh: () => Promise<string | null | void>) {
  // Keys of the actions running now, in start order. The ref is the synchronous guard; the state drives rendering.
  const activeRef = React.useRef(new Set<string>());
  const [active, setActive] = React.useState<string[]>([]);
  const [failure, setFailure] = React.useState<ActionFailure | null>(null);

  const runAction = React.useCallback<RunAction>(
    async (key, action, options) => {
      // A key that is already running is refused without a request, so the same change can never
      // be sent twice. Busy covers the mutation and the refresh after it.
      if (activeRef.current.has(key)) return false;
      activeRef.current.add(key);
      setActive([...activeRef.current]);
      setFailure(null);
      try {
        try {
          await action();
        } catch (err) {
          setFailure({ key, message: errorMessage(err, 'Action failed'), inline: options?.inline ?? false });
          return false;
        }
        // The change is saved. A failed refresh is a load problem, not a failed action:
        // reporting the action as failed would invite a retry that duplicates the change.
        let refreshError: string | null;
        try {
          refreshError = (await refresh()) ?? null;
        } catch (err) {
          refreshError = errorMessage(err, 'refresh failed');
        }
        if (refreshError !== null) {
          setFailure({ key: `${key}:refresh`, message: `Saved, but the dashboard could not be refreshed: ${refreshError}`, inline: false });
        }
        return true;
      } finally {
        activeRef.current.delete(key);
        setActive([...activeRef.current]);
      }
    },
    [refresh],
  );

  const clearFailure = React.useCallback(
    (key?: string) => setFailure((current) => (key === undefined || current?.key === key ? null : current)),
    [],
  );

  const isBusy = React.useCallback((key: string) => active.includes(key), [active]);
  /** The most recently started action that is still running, or null. Prefer `isBusy(key)`. */
  const busyAction = active.length > 0 ? active[active.length - 1] : null;

  return { busyAction, isBusy, runAction, failure, clearFailure };
}
