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

/** Provides the latest failure so `<ActionError>` can render it next to the form that caused it. */
export const ActionFailureContext = React.createContext<ActionFailure | null>(null);

export function useRunAction(refresh: () => Promise<void>) {
  const [busyAction, setBusyAction] = React.useState<string | null>(null);
  const [failure, setFailure] = React.useState<ActionFailure | null>(null);

  const runAction = React.useCallback<RunAction>(
    async (key, action, options) => {
      try {
        setBusyAction(key);
        setFailure(null);
        await action();
        await refresh();
        return true;
      } catch (err) {
        setFailure({ key, message: errorMessage(err, 'Action failed'), inline: options?.inline ?? false });
        return false;
      } finally {
        setBusyAction(null);
      }
    },
    [refresh],
  );

  const clearFailure = React.useCallback(() => setFailure(null), []);

  return { busyAction, runAction, failure, clearFailure };
}
