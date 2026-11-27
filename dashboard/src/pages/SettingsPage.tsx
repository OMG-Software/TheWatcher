import React from 'react';
import { fetchSettings, sendReport, updateSettings } from '../api';
import { ActionError } from '../components/ActionError';
import { InfoTip } from '../components/InfoTip';
import { LoadFailed } from '../components/LoadFailed';
import { usePolling } from '../hooks/usePolling';
import type { RunAction } from '../hooks/useRunAction';
import type { ServerSettings } from '../models';

type RetentionKey = 'metrics_retention_days' | 'status_history_retention_days' | 'alerts_retention_days' | 'log_matches_retention_days';

/** Retention settings with the ranges the server accepts. The last three exist only on servers with the retention lifecycle. */
const RETENTION_FIELDS: { key: RetentionKey; label: string; min: number; max: number; help: string }[] = [
  { key: 'metrics_retention_days', label: 'Metrics retention (days)', min: 1, max: 365, help: 'Metrics older than this are deleted by the hourly retention job.' },
  { key: 'status_history_retention_days', label: 'Status history retention (days)', min: 1, max: 3650, help: 'Status history older than this is deleted, except the newest row per indicator.' },
  { key: 'alerts_retention_days', label: 'Alerts retention (days)', min: 1, max: 3650, help: 'Acknowledged or archived alerts older than this are deleted; open alerts are kept. Also applies to expired silences and ended maintenance windows.' },
  { key: 'log_matches_retention_days', label: 'Log matches retention (days)', min: 1, max: 3650, help: 'Log matches older than this are deleted.' },
];

export function SettingsPage({
  runAction,
}: {
  runAction: RunAction;
}) {
  // Settings come from the server only. If they cannot be loaded there is no form at
  // all: showing built-in defaults would let "Save" overwrite the real settings.
  const { data: saved, error, refresh } = usePolling(fetchSettings);
  const [edits, setEdits] = React.useState<Partial<ServerSettings>>({});
  const [saving, setSaving] = React.useState(false); // the form is locked while a save runs, so nothing typed meanwhile is lost
  const draft = saved ? { ...saved, ...edits } : null;
  const setDraft = (update: (current: ServerSettings) => ServerSettings) =>
    setEdits((current) => (saved ? update({ ...saved, ...current }) : current));

  return (
    <main className="table-wrap management-wrap">
      <div className="management-header"><h1>Settings</h1></div>
      {!draft && !error ? <div className="banner">Loading settings...</div> : null}
      {!draft && error ? <LoadFailed what="settings" message={error} onRetry={() => void refresh()} /> : null}
      {draft ? (
        <form
          className="settings-form"
          onSubmit={(event) => {
            event.preventDefault();
            setSaving(true);
            void runAction('settings:save', () => updateSettings(draft), { inline: true })
              .then(async (ok) => {
                if (!ok) return; // keep the edits so the user can fix and retry
                await refresh();
                setEdits({});
              })
              .finally(() => setSaving(false));
          }}
        >
          <fieldset className="settings-fieldset" disabled={saving}>
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
              {RETENTION_FIELDS.map((field) => {
                const value = draft[field.key];
                // A server without the retention lifecycle neither reports nor stores the newer keys.
                const supported = value !== undefined;
                return (
                  <div className="field-with-tip" key={field.key}>
                    <label>
                      {field.label}
                      <input
                        disabled={!supported}
                        max={field.max}
                        min={field.min}
                        step={1}
                        type="number"
                        value={value ?? ''}
                        onChange={(e) => setDraft((d) => ({ ...d, [field.key]: Number(e.target.value) }))}
                      />
                    </label>
                    <InfoTip label={field.label} text={supported ? `${field.help} Allowed: ${field.min} to ${field.max}.` : `${field.help} This server version does not report this setting.`} />
                  </div>
                );
              })}
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
            <ActionError actionKey="settings:save" />
            <ActionError actionKey="reports:send" />
            <div className="button-row">
              <button className="text-button" type="submit">Save Settings</button>
              <button
                className="text-button"
                type="button"
                onClick={() => void runAction('reports:send', () => sendReport().then(() => undefined), { inline: true })}
              >
                Send Report Now
              </button>
            </div>
          </fieldset>
        </form>
      ) : null}
    </main>
  );
}

