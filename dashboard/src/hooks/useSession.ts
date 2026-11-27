import React from 'react';
import { fetchSession } from '../api';
import type { SessionInfo } from '../models';

/** Restores the signed-in session on page load; `session` stays null until login. */
export function useSession() {
  const [session, setSession] = React.useState<SessionInfo | null>(null);

  React.useEffect(() => {
    void fetchSession()
      .then(setSession)
      .catch(() => setSession(null));
  }, []);

  return { session, setSession };
}
