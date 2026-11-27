import React from 'react';

/** Runs a mutation, then refreshes; resolves true on success. Failures surface through the shared error banner. */
export type RunAction = (key: string, action: () => Promise<void>) => Promise<boolean>;

export function useRunAction(refresh: () => Promise<void>, setError: (message: string | null) => void) {
  const [busyAction, setBusyAction] = React.useState<string | null>(null);

  const runAction = React.useCallback<RunAction>(
    async (key, action) => {
      try {
        setBusyAction(key);
        setError(null);
        await action();
        await refresh();
        return true;
      } catch (err) {
        setError(err instanceof Error ? err.message : 'Action failed');
        return false;
      } finally {
        setBusyAction(null);
      }
    },
    [refresh, setError],
  );

  return { busyAction, runAction };
}
