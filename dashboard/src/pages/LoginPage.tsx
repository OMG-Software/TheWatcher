import React from 'react';
import { errorMessage, login } from '../api';
import type { SessionInfo } from '../models';
import { isInsecureOrigin } from '../uiLogic';

/** `notice` explains why the user is back here, for example an expired session. */
export function LoginPage({ notice = null, onLogin }: { notice?: string | null; onLogin: (session: SessionInfo) => void }) {
  const [username, setUsername] = React.useState('');
  const [password, setPassword] = React.useState('');
  const [error, setError] = React.useState<string | null>(null);
  return (
    <main className="login-wrap">
      <form
        className="login-panel"
        onSubmit={(event) => {
          event.preventDefault();
          void login(username, password).then(onLogin).catch((err) => setError(errorMessage(err, 'Login failed')));
        }}
      >
        <div className="logo login-logo">
          <span className="logo-dot" />
          THEWATCHER
        </div>
        {isInsecureOrigin(window.location) ? (
          <div className="banner error" role="alert">
            This page is served over plain HTTP. The session cookie is Secure, so the browser will drop it and you will
            be signed out straight away. Open the dashboard over HTTPS (or from localhost).
          </div>
        ) : null}
        {notice ? <div className="banner" role="status">{notice}</div> : null}
        {error ? <div className="banner error" role="alert">Login failed: {error}</div> : null}
        <label>
          Username
          <input value={username} onChange={(event) => setUsername(event.target.value)} />
        </label>
        <label>
          Password
          <input type="password" value={password} onChange={(event) => setPassword(event.target.value)} />
        </label>
        <button className="text-button" type="submit">Sign in</button>
      </form>
    </main>
  );
}
