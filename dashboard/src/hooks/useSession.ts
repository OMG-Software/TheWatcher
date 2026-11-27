import React from 'react';
import { fetchSession, logout, onUnauthorized } from '../api';
import type { SessionInfo } from '../models';

/**
 * Owns the signed-in session: restores it on page load, and returns to the
 * login screen (with `expired` set) when any API call reports 401.
 */
export function useSession() {
  const [session, setSession] = React.useState<SessionInfo | null>(null);
  const [expired, setExpired] = React.useState(false);
  const activeRef = React.useRef(false); // true while signed in, so late 401s after a deliberate logout are ignored
  const generationRef = React.useRef(0); // bumped on every session change, so a slow startup probe cannot overwrite a newer one

  React.useEffect(() => {
    const generation = generationRef.current;
    void fetchSession()
      .then((restored) => {
        if (generation !== generationRef.current) return;
        activeRef.current = true;
        setSession(restored);
      })
      .catch(() => {
        if (generation === generationRef.current) setSession(null);
      });
  }, []);

  // Registers the 401 listener now; the returned unsubscribe runs on cleanup.
  React.useEffect(
    () =>
      onUnauthorized(() => {
        if (!activeRef.current) return;
        generationRef.current++;
        activeRef.current = false;
        setExpired(true);
        setSession(null);
      }),
    [],
  );

  const signIn = React.useCallback((next: SessionInfo) => {
    generationRef.current++;
    activeRef.current = true;
    setExpired(false);
    setSession(next);
  }, []);

  const signOut = React.useCallback(() => {
    generationRef.current++;
    activeRef.current = false;
    setExpired(false);
    setSession(null);
    // The UI leaves immediately; a failed server call changes nothing the user can act on.
    void logout().catch(() => undefined);
  }, []);

  return { session, expired, signIn, signOut };
}
