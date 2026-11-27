# Dashboard

The dashboard is a React + TypeScript + Vite app under `dashboard/`.

## Role

The dashboard is the operator UI. It talks only to the server REST API; it does
not connect to agents.

Current capabilities:

- login/logout with SQLite-backed sessions;
- monitoring view of agent health and latest metrics;
- approved-agent-only monitoring;
- pending enrollment approval/rejection with group assignment on the Agents page;
- hostname-based alert notifications and alert acknowledgement/delete workflow;
- six-role user and group access control;
- per-agent collector configuration for poll interval, process limit, CPU,
  memory, disk, network, process watches, consecutive-reading counts,
  heartbeat interval, group assignment, and markdown runbooks;
- pause/resume maintenance mode;
- restart collectors;
- request immediate status;
- delete stale or rejected agent records.

## Development

Install dependencies:

```powershell
cd dashboard
npm.cmd install
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
serve this bundle yet. Host it separately for production use.

## Tests

```powershell
cd dashboard
npm.cmd test
```

The current tests cover health thresholds, alert display enrichment,
worst-status summary counts, missing-metrics behavior, maintenance state, agent
management API calls, grouped views, runbook saves, and role-aware user/group
API calls.

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

Pending enrollment rows show unapproved agents awaiting approval. Approving an
agent accepts a list of group ids; rejecting an agent marks enrollment
rejected. Approved/rejected buttons disappear from approved agent management
because decided enrollments are no longer pending.

Agent Management handles approved agents:

- approved agents show command buttons, then Configure, then Delete;
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
- approved agents can enter or leave maintenance mode with a stored reason and
  optional expiry timestamp;
- approved agents can restart collectors, request status, or disconnect;
- any agent row can be deleted.

### Alerts

The Alerts view lists active, non-deleted alerts visible to the current user's
groups. Operators and admins can acknowledge alerts or soft-delete them. The
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

The Views page supports group assignment when a view is created or edited.
Global account types can see all views and all group choices. Group account
types only receive their assigned groups from the API and only see views
assigned to those groups; public views from other groups are not returned.
Agents inside a view are still filtered by the same agent visibility rules.

## API Contract

[api.md](api.md) is the full reference (roles, bodies, error codes, limits).
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
| `DELETE /api/agents/:id` | Delete agent and metrics. |
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
3. Update `dashboard/src/main.tsx` if rendering or controls change.
4. Add/update Vitest coverage.
5. Run `npm.cmd run build` and `npm.cmd test`.
