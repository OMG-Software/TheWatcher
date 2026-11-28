import React from 'react';
import { ApiError, errorMessage, fetchSession, logout, markSessionChanged, onUnauthorized } from '../api';
import type { SessionInfo } from '../models';

/**
 * Owns the signed-in session: restores it on page load, and returns to the
 * login screen (with `expired` set) when any API call reports 401.
 *
 * `signOut()` keeps the user signed in until the server confirms: a 401 answer means the session
 * was already gone and counts as signed out; any other failure leaves the session active and sets
 * `signOutError`, because the server-side session (and its cookie) may still be valid and a reload
 * would restore it. Call `signOut()` again to retry.
 */
export function useSession() {
  const [session, setSession] = React.useState<SessionInfo | null>(null);
  const [expired, setExpired] = React.useState(false);
  const [signingOut, setSigningOut] = React.useState(false);
  const [signOutError, setSignOutError] = React.useState<string | null>(null);
  const activeRef = React.useRef(false); // true while signed in, so late 401s after a deliberate logout are ignored
  const signingOutRef = React.useRef(false); // synchronous guard so two clicks send one request
  const generationRef = React.useRef(0); // bumped on every session change, so a slow startup probe cannot overwrite a newer one

  // Every change of session also tells the API client, so a 401 from a request that started
  // under the old session cannot end the new one.
  const changeSession = React.useCallback(() => {
    generationRef.current++;
    markSessionChanged();
  }, []);

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
        changeSession();
        activeRef.current = false;
        setExpired(true);
        setSignOutError(null);
        setSession(null);
      }),
    [changeSession],
  );

  const signIn = React.useCallback(
    (next: SessionInfo) => {
      changeSession();
      activeRef.current = true;
      setExpired(false);
      setSignOutError(null);
      setSession(next);
    },
    [changeSession],
  );

  const signOut = React.useCallback(async (): Promise<void> => {
    if (signingOutRef.current) return;
    signingOutRef.current = true;
    const generation = generationRef.current;
    setSigningOut(true);
    setSignOutError(null);
    try {
      await logout();
    } catch (err) {
      // Something else (a 401 elsewhere, a new sign-in) already changed the session: nothing left to report.
      if (generation !== generationRef.current) return;
      // 401: the server no longer knows the session, which is what signing out wants.
      if (!(err instanceof ApiError && err.status === 401)) {
        setSignOutError(`Sign out failed, and your session is still active: ${errorMessage(err, 'the server could not be reached')}.`);
        return;
      }
    } finally {
      signingOutRef.current = false;
      setSigningOut(false);
    }
    if (generation !== generationRef.current) return;
    changeSession();
    activeRef.current = false;
    setExpired(false);
    setSession(null);
  }, [changeSession]);

  const dismissSignOutError = React.useCallback(() => setSignOutError(null), []);

  return { session, expired, signingOut, signOutError, signIn, signOut, dismissSignOutError };
}
