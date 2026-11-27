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

export function useRunAction(refresh: () => Promise<void>) {
  const [busyAction, setBusyAction] = React.useState<string | null>(null);
  const [failure, setFailure] = React.useState<ActionFailure | null>(null);

  const runAction = React.useCallback<RunAction>(
    async (key, action, options) => {
      setBusyAction(key);
      setFailure(null);
      try {
        await action();
      } catch (err) {
        setFailure({ key, message: errorMessage(err, 'Action failed'), inline: options?.inline ?? false });
        return false;
      } finally {
        setBusyAction(null);
      }
      // The change is saved. A failed refresh is a load problem, not a failed action:
      // reporting the action as failed would invite a retry that duplicates the change.
      try {
        await refresh();
      } catch (err) {
        setFailure({ key: `${key}:refresh`, message: `Saved, but the dashboard could not be refreshed: ${errorMessage(err, 'refresh failed')}`, inline: false });
      }
      return true;
    },
    [refresh],
  );

  const clearFailure = React.useCallback(
    (key?: string) => setFailure((current) => (key === undefined || current?.key === key ? null : current)),
    [],
  );

  return { busyAction, runAction, failure, clearFailure };
}
