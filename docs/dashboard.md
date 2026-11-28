# Dashboard

The dashboard is a React + TypeScript + Vite app under `dashboard/`.

## Role

The dashboard is the operator UI. It talks only to the server REST API; it does
not connect to agents.

Current capabilities:

- login/logout with SQLite-backed sessions (the session cookie is `Secure`, so
  the dashboard must be served over HTTPS, or from `localhost`, for login to
  persist);
- monitoring view of agent health and latest metrics;
- approved-agent-only monitoring;
- pending enrollment approval/rejection on the Agents page (an approved agent
  is currently assigned to the first group in the group list, or to no group
  when none exist; there is no group picker yet);
- hostname-based alert notifications, alert acknowledgement (with a note) and
  archive workflow;
- custom Views, Silences, Maintenance windows and Settings pages;
- six-role user and group access control;
- per-agent collector configuration for poll interval, process limit, CPU,
  memory, disk, network, process watches, consecutive-reading counts,
  heartbeat interval, group assignment, and markdown runbooks;
- enter or resume maintenance mode for one agent;
- restart collectors;
- request immediate status;
- delete approved agent records.

## Source Layout

```text
dashboard/src/main.tsx          entry point: mounts <App/>
dashboard/src/App.tsx           app shell: session gate, top bar, summary cards, page routing
dashboard/src/pages/            one file per page (Monitoring, Views, Agents, Alerts,
                                Silences, Maintenance, Users, Runbooks, Settings, Login)
dashboard/src/components/       shared components and modals (ActionButton, StatusDot,
                                AgentConfigModal, AcknowledgeModal, RunbookModal, ...)
dashboard/src/hooks/            usePolling, useDashboardData, useRunAction, useSession, useActiveView
dashboard/src/testing/          fake REST server for UI tests
dashboard/src/permissions.ts    role helpers: isGlobal, canOperate, canManageUsers, canManageGroups
dashboard/src/api.ts            REST client
dashboard/src/models.ts         API row types
dashboard/src/status.ts         health classification and view models
dashboard/src/uiLogic.ts        navigation, labels and markdown helpers
dashboard/src/styles.css        all styling
```

Roles decide which controls are shown (the server enforces every permission):

| Helper | True for |
| --- | --- |
| `isGlobal` | `global_admin`, `global_operator`, `global_viewer` |
| `canOperate` | admins and operators, global or group |
| `canManageUsers` | `global_admin`, `group_admin` |
| `canManageGroups` | `global_admin` |

## Linting

```powershell
cd dashboard
npm.cmd run lint
```

ESLint (flat config, `typescript-eslint`, `eslint-plugin-react-hooks`) runs in
CI in the `dashboard` job together with `npm run build`, `npm test` and
`npm audit --omit=dev`. CI installs with `npm ci --ignore-scripts`, so no
package's install lifecycle script runs; a new dependency that only works
after its `postinstall` script will fail CI. The footer shows the dashboard version, injected from
`dashboard/package.json` at build time.

## Development

Install dependencies:

```powershell
cd dashboard
npm.cmd ci
```

Run the dev server:

```powershell
npm.cmd run dev
```

Vite serves the dashboard at `http://127.0.0.1:5173` and proxies `/api` to
`http://127.0.0.1:8080`.

## Build

```powershell
cd dashboard
npm.cmd run build
```

The production bundle is written to `dashboard/dist/`. The C++ server does not
serve this bundle: it only exposes the `/api` REST endpoints. Host `dist/`
separately (for example behind a reverse proxy that also forwards `/api` to the
server) over HTTPS, because the session cookie is `Secure`. Set
`VITE_API_BASE_URL` at build time if the API lives on another origin.

## Errors, session expiry and polling

- **API errors.** `api.ts` throws `ApiError` (HTTP `status` plus the server's
  `{"error": "..."}` message, or the status line when there is no body).
  `errorMessage(err, fallback)` turns any caught value into text.
- **Failed actions.** `runAction(key, fn, { inline })` resolves `true`/`false`.
  A failure shows the server's message in a dismissible banner under the
  summary cards, or, with `{ inline: true }`, next to the form or dialog that
  started it via `<ActionError actionKey="...">`. Forms and dialogs clear or
  close only when `runAction` resolves `true`, so input survives a failed save.
  `true` means the mutation itself succeeded: if the refresh afterwards fails,
  that is reported in the page banner ("Saved, but the dashboard could not be
  refreshed"), never as a failed action, so a retry cannot duplicate the change.
  A dialog or form with a Cancel (acknowledge, agent configuration, password,
  view editor) cannot be dismissed while its request runs, because the failure
  would have nowhere to show, and it drops its own failure when closed, so
  reopening it does not show an old error (`clear(key)` from
  `ActionFailureContext`). Settings disables its form while a save runs, so
  nothing typed meanwhile is lost.
- **Failed loads are not "no data".** If the first load of the required data
  fails, the page is replaced by "Could not load dashboard data" with Retry. If
  a later refresh fails, the old data stays under a "Could not refresh" banner.
  Optional sections (alerts, silences, maintenance windows, users, views,
  pending enrollments, groups) load independently: a 403 means the role may not
  see them (empty); any other failure is flagged and the last good data is kept.
  A failed group list is flagged on every page that uses groups (Monitoring,
  Views, Agents, Users). A page that loads for itself (runbooks, settings,
  archived alerts) shows "Loading..." until its first request settles, never
  its empty state. The Monitoring detail row loads metric history and log
  matches independently, each with its own error.
  Settings shows an error instead of a form when it cannot load, so defaults can
  never be saved over the real settings.
- **Session expiry.** Any API call that returns 401 (except the login and
  session probes) tells `useSession`, which returns to the login screen with
  "Your session expired" and unmounts the dashboard, which stops all polling.
  The login page warns when served over plain HTTP from a non-local host: the
  session cookie is `Secure`, so the browser drops it there.
- **Polling.** `usePolling(fetcher, { intervalMs, enabled })` keeps at most one
  request in flight and skips a tick while one runs. `refresh()` during an
  in-flight request queues exactly one follow-up load, which starts when the
  current one settles (the running one may predate the change that prompted the
  refresh); further `refresh()` calls share that follow-up. Disabling or
  unmounting ignores the in-flight result and drops the queued follow-up. The
  poll pauses while the tab is hidden and refreshes as soon as it is visible
  again. `useDashboardData` polls every 5 s with it. `refresh()` resolves to
  the load error message of a load that started after the call (null on
  success) and never rejects. `runAction` tracks every running action by key: a
  key that is already running is refused (resolves `false`, no request), and
  finishing removes only its own key. `isBusy(key)` tells whether one runs;
  `busyAction` is the most recently started running key, kept for
  compatibility. `runAction` stays busy until the mutation and
  its refresh have both settled, so a control guarded by `busyAction` cannot
  repeat the change while the result loads, and it uses the refresh result to
  report a failed post-action refresh. An optional `merge(next, previous)`
  combines an accepted result with the previous data and never sees a stale
  one; `useDashboardData` uses it to keep the last good value of a failed
  section. `useSession` ignores a slow startup session probe that settles
  after a sign-in or sign-out. After a successful settings save whose reload
  fails, the form keeps the saved values and shows a "Could not refresh
  settings" warning. There is no request timeout in `api.ts`, so a hung request
  keeps its action busy, as a hung mutation always has.

## Tests

```powershell
cd dashboard
npm.cmd test
```

The current tests cover health thresholds, alert display enrichment,
worst-status summary counts, missing-metrics behavior, maintenance state, agent
management API calls, grouped views, role helpers, navigation and label logic,
and role-aware user/group API calls. Tests are collected from
`src/**/*.test.ts(x)` only; `tsc` never writes into `src`.

UI behaviour tests (`*.test.tsx`) use Vitest with jsdom and React Testing
Library. `src/testing/fakeApi.ts` is a fake REST server: routes are keyed
`"METHOD /path"`, unlisted routes answer an empty list, and `calls` records
requests. Tests are written as GIVEN/WHEN/THEN `describe`/`it` names. They cover
role gating, action and load errors, 401 to login, the polling guard and
hidden-tab pause (fake timers), Settings after a failed load and forms after a
failed save. These dev dependencies install without install scripts
(`npm ci --ignore-scripts`, as CI does).

## Views

### Monitoring

The Monitoring view shows approved agents only. It joins `/api/agents`,
`/api/metrics`, and `/api/alerts/unacknowledged` by `agent_id`.

Approved agents are grouped by their assigned group names. Global account types
see all approved agents they can access, with each group rendered as a section
heading above its agents. Agents that belong to multiple groups appear in each
matching group section. Agents without an assigned or visible group appear in
the `Ungrouped` section. Group account types do not get cross-group filters or
ungrouped options; the API only returns their assigned group data.

The group filter can show all groups, a single group, or only ungrouped agents
for global account types.

Columns:

```text
Server | Uptime | Status | Alerts | CPU | Memory | Disk | Network | Proc | Heartbeat
```

The top counters are:

```text
Healthy | Warning | Degraded | Critical | Maintenance | Offline
```

Status priority is red, amber, yellow, grey, then green. A host with no data is
shown as yellow in the derived Status column while its missing component dots
are grey. Maintenance turns all component dots blue.

The server identity cell shows the hostname and primary non-loopback IP address
when the agent reports one. The Heartbeat dot is the final component dot. It is
green when the server still considers the agent connected, grey when the agent
is offline or has never sent a heartbeat, and blue during maintenance.

Unacknowledged alerts appear between the top counters and the Monitoring table.
Each alert is rendered as its own severity-themed item using the known hostname
as primary text and the agent id as smaller secondary text. Non-global users
only see alerts for agents assigned to their groups; changing an agent's group
changes which users can see its alerts.

### Agents

The Agents view starts with Pending Enrollments for global operators and global
admins, then shows Agent Management below it.

Pending enrollment rows show unapproved agents awaiting approval. Each row has
a Group picker; it starts at "No group (global users only)" and the operator has
to pick a group on purpose, because the approval names its groups explicitly. Reject
marks the enrollment rejected. Approve/Reject buttons disappear once an
enrollment is decided.

Agent Management handles approved agents:

- the Configure modal's Access section lists one checkbox per group, so an agent
  in several groups keeps every membership when saved; group users see them
  read-only. Saving sends the `config_revision` the modal opened with; if
  someone else saved the agent in the meantime the server answers 409 and the
  modal shows the message (close and reopen it to load their changes);
- approved agents show command buttons, then Configure, then Delete (Delete is
  shown to global and group admins only; operators do not see it);
- approved agent identity shows the hostname, primary non-loopback IP address,
  and agent id;
- the Configure modal updates heartbeat interval, poll interval, process sample
  limit, group assignment, per-agent markdown runbook, CPU and memory absolute
  percentage thresholds, fixed disk thresholds, network interface Mbps
  thresholds, process watches, and consecutive-reading counts;
- each Configure field has an info tooltip that describes the setting;
- saving the Configure modal persists the whole collector config and closes the
  modal on success;
- disk rows are shown by mount point/path with device name in brackets;
- disk and network rows stack the enable toggle, thresholds, and anomaly inputs
  vertically for readability;
- network thresholds are combined receive plus transmit megabits per second per
  selected interface;
- process watches use exact executable names plus an expected process count;
- the Maintenance button enters maintenance with the fixed reason
  `operator request` and no expiry; for a reason or an end time use the
  Maintenance page. The button reads Resume while the agent is in maintenance;
- approved agents can restart collectors or request status (there is no
  Disconnect button);
- Restart, Status, Maintenance, Configure and Delete are enabled only for
  operators and admins, and only for approved, non-rejected agents.

### Alerts

The Alerts view lists active, non-deleted alerts visible to the current user's
groups. Operators and admins can acknowledge alerts (with an optional note),
acknowledge and archive them in one step, or archive them; the list supports
agent and status filters, bulk selection, and a Show archived toggle. Archiving
is a soft delete on the server. The
Monitoring alert dot is green unless the host has an unacknowledged alert, in
which case it is red. Monitoring alert notifications and Alert rows show the
known hostname as primary text and the agent id as smaller secondary text.
Alert rows can display the agent's markdown runbook.
The Runbook action opens a modal that renders the agent markdown as headings,
paragraphs, lists, code blocks, and safe links.

### Users & Groups

The Users & Groups view lists users, account Type values, groups, and state
visible to the current user. Global admins can create groups and any account
type. Group admins can create users only inside their own groups and cannot
assign global account types. Global and group operators have read-only user
access.

Account types:

| Type | Scope |
| --- | --- |
| `global_admin` | Full access to every agent, view, alert, setting, user, and group. |
| `global_operator` | Full operational access; users and groups are read-only. |
| `global_viewer` | Read-only access to everything. |
| `group_admin` | Full access inside assigned groups plus user creation inside those groups. |
| `group_operator` | Operational access inside assigned groups; users are read-only. |
| `group_viewer` | Read-only access inside assigned groups. |

### Views

The Views page lists the custom views the API returns for the current user,
with name, owner, agent count, Public/Private flag, group and creation date.
Open applies the view as a filter on the Monitoring page (the URL hash becomes
`#view-<id>`, so it can be bookmarked). Operators can create views (name,
agents, Public flag, group). Edit and Delete are shown to global operators and
admins, to the view's owner, and to group admins and operators for views of their
own groups.

The Views page supports group assignment when a view is created or edited.
Global account types can see all views and all group choices ("No group" is
only offered to them). Group account types see the views of their groups, and
public or own views that have no group; public views of another group are not
shown. The agent list of a view only contains the agents the user can access.
A `#view-<id>` bookmark to a view that no longer exists, or that the user cannot
open, is dropped with a notice instead of filtering the page to nothing. The
full policy is in [authorization.md](authorization.md).

### Silences

Visible to operators and admins. A silence suppresses alerts for one agent (or
all agents, global accounts only) and one indicator (or all indicators) for a
number of hours, with a reason. The table lists agent, indicator, reason,
expiry and creator; operators can delete a silence.

### Maintenance

Visible to operators and admins. Schedules a maintenance window for one agent
(or all agents, global accounts only) from a start date and time for a number
of hours, with a reason. The table lists agent, start, end, reason and creator;
operators can delete a window. Times are entered and shown in the browser's
local time zone.

### Settings

Visible to global operators and admins only. Edits the webhook URL, offline
threshold (seconds), escalation timeout (seconds), metrics retention (days) and
the scheduled digest report (enabled, daily or weekly schedule, day of week,
report webhook URL). Save Settings persists them; Send Report Now sends the
report immediately.

Webhook URLs can carry a secret, so only the global admin sees and edits them.
A global operator sees both URLs masked to scheme and host
(`https://hooks.slack.com/…`), read-only with a short note, and Save Settings
leaves the webhook fields out of the request.

## API Contract

The dashboard consumes:

| Endpoint | Purpose |
| --- | --- |
| `POST /api/login` | Create a session for a local SQLite user. |
| `POST /api/logout` | Delete the current session. |
| `GET /api/session` | Return the current authenticated session. |
| `GET /api/agents` | Approved agent identity, connection, maintenance, settings, groups, and timestamps. |
| `GET /api/pending-enrollments` | Pending enrollment records for global operators and admins. |
| `GET /api/metrics` | Latest metrics snapshot per agent. |
| `GET /api/groups` | List groups. |
| `POST /api/groups` | Create a group. |
| `GET /api/users` | List users and group memberships. |
| `POST /api/users` | Create a user with hashed password, role, and group memberships. |
| `GET /api/alerts` | List active, non-deleted alerts. |
| `GET /api/alerts/unacknowledged` | List active unacknowledged alerts. |
| `POST /api/alerts/:id/ack` | Acknowledge an alert. |
| `DELETE /api/alerts/:id` | Soft-delete an alert. |
| `POST /api/agents/:id/approve` | Approve pending enrollment and assign groups. |
| `POST /api/agents/:id/reject` | Reject pending enrollment. |
| `POST /api/agents/:id/groups` | Replace an approved agent's group memberships. |
| `DELETE /api/agents/:id` | Delete agent and metrics, and revoke its key; admins only (group admins for agents of their groups). |
| `POST /api/agents/:id/set_interval` | Legacy endpoint for poll interval update. |
| `POST /api/agents/:id/set_process_limit` | Legacy endpoint for process limit update. |
| `POST /api/agents/:id/thresholds` | Legacy endpoint for old threshold rows. |
| `POST /api/agents/:id/collector_config` | Persist collection interval, heartbeat interval, process limit, groups, runbook, and full collector configuration. |
| `POST /api/agents/:id/runbook` | Persist the agent markdown runbook. |
| `POST /api/agents/:id/maintenance` | Enter maintenance with optional reason and expiry. |
| `POST /api/agents/:id/pause` | Queue maintenance pause. |
| `POST /api/agents/:id/resume` | Queue maintenance resume. |
| `POST /api/agents/:id/restart_collectors` | Queue collector restart. |
| `POST /api/agents/:id/get_status` | Queue immediate metrics report. |
| `GET /api/views` | List views visible to the current user. |
| `POST /api/views` | Create a view with optional `group_id`. |
| `GET /api/views/:id` | Read a visible view. |
| `PUT /api/views/:id` | Update a view's name, agents, visibility, or group. |
| `DELETE /api/views/:id` | Delete an editable view. |
| `GET /api/runbooks` | List legacy global runbook links. |
| `POST /api/runbooks` | Create a legacy global runbook link; `global_admin` only. |
| `DELETE /api/runbooks/:id` | Delete a legacy global runbook link; `global_admin` only. |

Agent rows include:

```text
agent_id
hostname
platform
curve_public_key_z85
approved
rejected
connected
maintenance
maintenance_reason
maintenance_until
collection_interval
heartbeat_interval
process_limit
runbook_markdown
collector_config
thresholds
first_seen
last_seen
group_ids
```

Metric rows include:

```text
agent_id
timestamp_ms
metrics
```

`metrics` is the JSON form of `common/metrics.hpp::SystemMetrics`.

## State Rules

- Pending agents receive `approved=false` until an operator approves them.
- Approved enrollment responses include the server public key and pinned
  fingerprint the agent persists before connecting to the data socket.
- Rejected agents receive an enrollment rejection on the next attempt.
- CPU and memory use absolute per-agent percentage thresholds. Defaults are
  warning 80, degraded 90, critical 95.
- Disk checks are per fixed disk. Empty disk config means monitor all reported
  fixed disks with default thresholds.
- Network checks are per selected interface using combined receive plus transmit
  megabits per second. Defaults are warning 100, degraded 200, critical 300.
- Process checks use exact executable names and expected counts. Fewer running
  instances than expected escalates yellow, amber, then red across configured
  consecutive failed readings.
- Temperature collection is no longer evaluated by the dashboard or status
  engine. Historical temperature fields can still decode from old metrics.
- Worsening numeric collector transitions are committed only after the
  configured number of consecutive readings. Recovery resets pending counts.
- `connected` becomes true after inbound agent traffic.
- `connected` becomes false after successful disconnect ACK or after
  `offline_after_seconds` with no inbound frame.
- `maintenance` becomes true from the maintenance action or pause ACK.
- `maintenance` becomes false after successful resume ACK.
- Maintenance suppresses and clears active alerts for that host.
- Worsening indicator transitions create alerts. Recoveries are history-only.
- Alert delete is a soft delete; alert history remains in SQLite.
- Runtime settings are persisted on the server and returned to agents after
  each metrics submission via config refresh.

## Changing Dashboard Data

When server API fields change:

1. Update `dashboard/src/models.ts`.
2. Update `dashboard/src/status.ts` if health or grouping changes.
3. Update the matching file under `dashboard/src/pages/` or
   `dashboard/src/components/` if rendering or controls change.
4. Add/update Vitest coverage.
5. Run `npm.cmd run build` and `npm.cmd test`.
