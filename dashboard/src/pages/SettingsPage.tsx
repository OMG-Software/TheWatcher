import React from 'react';
import { fetchSettings, sendReport, updateSettings } from '../api';
import type { RunAction } from '../hooks/useRunAction';
import type { ServerSettings } from '../models';

export function SettingsPage({
  runAction,
}: {
  runAction: RunAction;
}) {
  const [draft, setDraft] = React.useState<ServerSettings>({ webhook_url: '', offline_after_seconds: 120, escalation_timeout_seconds: 300, metrics_retention_days: 30, reports_enabled: false, reports_schedule: 'daily', reports_hour: 8, reports_day_of_week: 1, reports_webhook_url: '' });
  const [loaded, setLoaded] = React.useState(false);

  React.useEffect(() => {
    void fetchSettings().then((s) => {
      setDraft(s);
      setLoaded(true);
    }).catch(() => setLoaded(true));
  }, []);

  return (
    <main className="table-wrap management-wrap">
      <div className="management-header"><h1>Settings</h1></div>
      {!loaded ? <div className="banner">Loading settings...</div> : null}
      {loaded ? (
        <form
          className="settings-form"
          onSubmit={(event) => {
            event.preventDefault();
            void runAction('settings:save', () => updateSettings(draft));
          }}
        >
          <div className="form-grid">
            <label>
              Webhook URL
              <input
                placeholder="https://hooks.example.com/…"
                type="url"
                value={draft.webhook_url}
                onChange={(e) => setDraft((d) => ({ ...d, webhook_url: e.target.value }))}
              />
            </label>
            <label>
              Offline after (seconds)
              <input
                min={10}
                step={1}
                type="number"
                value={draft.offline_after_seconds}
                onChange={(e) => setDraft((d) => ({ ...d, offline_after_seconds: Number(e.target.value) }))}
              />
            </label>
            <label>
              Escalation timeout (seconds)
              <input
                min={60}
                step={1}
                type="number"
                value={draft.escalation_timeout_seconds}
                onChange={(e) => setDraft((d) => ({ ...d, escalation_timeout_seconds: Number(e.target.value) }))}
              />
            </label>
            <label>
              Metrics retention (days)
              <input
                min={1}
                max={365}
                step={1}
                type="number"
                value={draft.metrics_retention_days}
                onChange={(e) => setDraft((d) => ({ ...d, metrics_retention_days: Number(e.target.value) }))}
              />
            </label>
          </div>
          <h2>Scheduled Reports</h2>
          <div className="form-grid">
            <label>
              Enable digest reports
              <input
                type="checkbox"
                checked={draft.reports_enabled}
                onChange={(e) => setDraft((d) => ({ ...d, reports_enabled: e.target.checked }))}
              />
            </label>
            <label>
              Schedule
              <select
                value={draft.reports_schedule}
                onChange={(e) => setDraft((d) => ({ ...d, reports_schedule: e.target.value as 'daily' | 'weekly' }))}
              >
                <option value="daily">Daily</option>
                <option value="weekly">Weekly</option>
              </select>
            </label>
            <label>
              Hour (UTC, 0–23)
              <input
                min={0}
                max={23}
                step={1}
                type="number"
                value={draft.reports_hour}
                onChange={(e) => setDraft((d) => ({ ...d, reports_hour: Number(e.target.value) }))}
              />
            </label>
            {draft.reports_schedule === 'weekly' ? (
              <label>
                Day of week
                <select
                  value={draft.reports_day_of_week}
                  onChange={(e) => setDraft((d) => ({ ...d, reports_day_of_week: Number(e.target.value) }))}
                >
                  <option value={0}>Sunday</option>
                  <option value={1}>Monday</option>
                  <option value={2}>Tuesday</option>
                  <option value={3}>Wednesday</option>
                  <option value={4}>Thursday</option>
                  <option value={5}>Friday</option>
                  <option value={6}>Saturday</option>
                </select>
              </label>
            ) : null}
            <label>
              Report webhook URL
              <input
                placeholder="https://hooks.example.com/…"
                type="url"
                value={draft.reports_webhook_url}
                onChange={(e) => setDraft((d) => ({ ...d, reports_webhook_url: e.target.value }))}
              />
            </label>
          </div>
          <div className="button-row">
            <button className="text-button" type="submit">Save Settings</button>
            <button
              className="text-button"
              type="button"
              onClick={() => void runAction('reports:send', () => sendReport().then(() => undefined))}
            >
              Send Report Now
            </button>
          </div>
        </form>
      ) : null}
    </main>
  );
}

