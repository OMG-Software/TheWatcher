import React from 'react';
import { login } from '../api';
import type { SessionInfo } from '../models';

export function LoginPage({ onLogin }: { onLogin: (session: SessionInfo) => void }) {
  const [username, setUsername] = React.useState('');
  const [password, setPassword] = React.useState('');
  const [error, setError] = React.useState<string | null>(null);
  return (
    <main className="login-wrap">
      <form
        className="login-panel"
        onSubmit={(event) => {
          event.preventDefault();
          void login(username, password).then(onLogin).catch((err) => setError(err instanceof Error ? err.message : 'Login failed'));
        }}
      >
        <div className="logo login-logo">
          <span className="logo-dot" />
          THEWATCHER
        </div>
        {error ? <div className="banner error">Login failed: {error}</div> : null}
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

