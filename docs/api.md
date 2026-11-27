# REST API Reference

The server's REST API is implemented in `server/api.cpp` and consumed by the
dashboard. Input limits live in `server/api_validation.hpp`. This page lists
every route, the role it needs, what it accepts and returns, and the errors it
can answer with.

## Conventions

### Authentication

`POST /api/login` sets an `HttpOnly` cookie named `tw_session` (8 hour
lifetime). Every other route except login needs that cookie and answers `401`
without it. Unsafe methods (`POST`, `PUT`, `DELETE`) are rejected with `403`
when an `Origin` or `Referer` header names a different origin than the API
(loopback dashboards on another port are allowed).

### Roles

| Role needed | Who passes |
| --- | --- |
| viewer | any signed-in user |
| operator | `global_admin`, `global_operator`, `group_admin`, `group_operator` |
| admin | `global_admin`, `group_admin` |
| global | `global_admin`, `global_operator`, `global_viewer` |
| global admin | `global_admin` |

Group-scoped users (`group_*` roles) only see and change agents that share a
group with them. For routes marked "agent access", a group user who is outside
the agent's groups gets `403`. The legacy role names `admin`, `operator` and
`viewer` are accepted and mapped to the `global_*` roles.

### Requests and responses

- Request bodies are JSON. Routes that write data answer `415` when a
  `Content-Type` header is present but is not `application/json`.
- Success responses are JSON: `{"ok": true}`, the created id, or the resource.
- Ids in a path (`:id` for alerts, windows, silences, runbooks, views, users)
  are positive decimal integers. Agent ids are opaque strings.

### Errors

Every failure uses one shape: `{"error": "<message>"}` with one of:

| Status | Meaning |
| --- | --- |
| 400 | Malformed or out-of-range input: a non-numeric id, an invalid JSON body, a missing field, a value over its limit. |
| 401 | No valid session, or wrong credentials. |
| 403 | Signed in but not allowed (role, group scope, cross-origin request). |
| 404 | The id in the path, or the agent or group id named in the body, does not exist. |
| 409 | The request conflicts with existing data (duplicate user or group name, public key already bound to another agent). |
| 415 | `Content-Type` is not JSON. |
| 429 | Login throttled. |
| 500 | Server fault. The message is always `internal error`; the cause is in the server log. |

Validation failures are always `400` (the API does not use `422`). A group user
asking for an agent that does not exist gets `403`, the same as for an agent in
another group, so ids cannot be probed; global users get `404`.

### Limits

| Value | Limit |
| --- | --- |
| Agent id | 128 bytes |
| Group, user, view and indicator names | 64 bytes |
| Password | 256 bytes |
| Reasons (maintenance, silence) | 1024 bytes |
| Notes, descriptions, runbook notes | 4096 bytes (an alert acknowledgement note is cut to 4096 bytes on a UTF-8 boundary instead of rejected) |
| URLs (runbook, webhooks) | 2048 bytes |
| Agent runbook markdown | 65536 bytes |
| `alert_ids`, view `agent_ids` | 1000 entries |
| `group_ids` | 256 entries |
| Disks, networks, processes, logs in a collector config | 64 entries each |
| `*_readings` in a collector config | 1 to 1000 |
| Collection interval, `interval_seconds` | 1 to 86400 seconds |
| `heartbeat_interval` | 1 to 60 seconds |
| `process_limit`, `limit` of `set_process_limit` | 1 to 1000 |
| Timestamps (`until_ms`, `start_ms`, `end_ms`) | 1 to 4102444800000 (year 2100) |

### Paging and query values

Query integers must be plain numbers. A malformed or non-positive value is
`400`; a value above the route's maximum is clamped to the maximum.

| Route | Parameter | Default | Maximum |
| --- | --- | --- | --- |
| `GET /api/alerts`, `GET /api/alerts/unacknowledged` | `limit`, `offset` | 1000, 0 | 1000 |
| `GET /api/metrics/:id` | `limit` | 100 | 1000 |
| `GET /api/agents/:id/history` | `limit` | 100 | 200 |
| `GET /api/agents/:id/log-matches` | `limit` | 200 | 1000 |
| `GET /api/uptime/:id` | `days` | 7 | 90 |

Alerts are newest first. Filtering by the caller's groups, `limit` and `offset`
run inside the database, so a request costs one query however many alerts exist.

## Session

| Route | Role | Request | Response and errors |
| --- | --- | --- | --- |
| `POST /api/login` | none | `{username, password}` | `200 {username, role}` and the session cookie. `400` bad body or over-long credentials, `401` invalid credentials, `429` throttled. |
| `POST /api/logout` | none | none | `200 {ok}`; clears the cookie. |
| `GET /api/session` | session | none | `200 {username, role}`. |

## Groups and users

| Route | Role | Request | Response and errors |
| --- | --- | --- | --- |
| `GET /api/groups` | viewer | none | Array of `{group_id, name, built_in}`; group users see their own groups. |
| `POST /api/groups` | global admin | `{name}` | `200 {group_id}`. `409` name exists. |
| `GET /api/users` | viewer | none | Array of `{user_id, username, role, built_in, disabled, group_ids}`; group users see users that share a group. |
| `POST /api/users` | admin | `{username, password, role?, group_ids?}` (`role` defaults to `group_viewer`) | `200 {user_id}`. `400` invalid role or over a limit or group admin creating a global user, `404` unknown group, `409` username exists. Group admins can only assign their own groups. |
| `PUT /api/users/:id/disable` | global admin | none | `{ok}`. `404` unknown or built-in user. |
| `PUT /api/users/:id/enable` | global admin | none | `{ok}`. `404` unknown user. |
| `DELETE /api/users/:id` | global admin | none | `{ok}`. `404` unknown or built-in user. |
| `PUT /api/users/:id/password` | viewer; own account, or global admin for any | `{password}` | `{ok}`. `403` another user's account, `404` unknown user. |

## Agents

"Agent access" means the caller must pass the group scope check described
under Roles. `404` is returned when the agent does not exist.

| Route | Role | Request | Response and errors |
| --- | --- | --- | --- |
| `GET /api/pending-enrollments` | operator and global | none | Array of agent records awaiting approval. |
| `GET /api/agents` | viewer | none | Array of approved agents the caller can see, each with `group_ids`. |
| `POST /api/agents/:id/approve` | operator and global | optional `{group_ids}` (defaults to the Admins group) | `{ok}`. `404` unknown agent or group, `409` public key already bound to another approved agent. The body is validated before the agent is approved. |
| `POST /api/agents/:id/reject` | operator and global | none | `{ok}`; removes the key from ZAP. `404`. |
| `POST /api/agents/:id/groups` | operator and global | `{group_ids}` | `{ok}`. `404` unknown agent or group. |
| `DELETE /api/agents/:id` | operator, agent access | none | `{ok}`. Removes the agent, its metrics, alerts and group links, and in one transaction its silences, maintenance windows, agent runbooks and its id in views, so a re-enrolled agent starts clean. |
| `POST /api/agents/:id/command` | operator, agent access | `{command_type, args?}` (`args` is a byte array, at most 4096) | `{ok, command_id}`. `400` unknown `command_type`. |
| `POST /api/agents/:id/set_interval` | operator, agent access | `{interval_seconds}` | `{ok}`; saved and sent to the agent. |
| `POST /api/agents/:id/set_process_limit` | operator, agent access | `{limit}` | `{ok}`; saved and sent to the agent. |
| `POST /api/agents/:id/thresholds` | operator, agent access | `{thresholds: {cpu, memory, disk, network: {warning_pct_of_avg, degraded_pct_of_avg, critical_pct_of_avg}}}`, values finite, above zero and increasing | `{ok}`. |
| `POST /api/agents/:id/collector_config` | operator, agent access | `{collection_interval, process_limit, heartbeat_interval?, collector_config, runbook_markdown?, group_ids?}` | `{ok}`. Only a global admin may add, edit or remove log monitors (`403`); `group_ids` is applied only for global roles. |
| `POST /api/agents/:id/maintenance` | operator, agent access | `{reason?, until_ms?}` (`until_ms` 0 or omitted means until resumed) | `{ok}`; pauses the agent and clears its active alerts. |
| `POST /api/agents/:id/pause` | operator, agent access | none | `{ok}`; enters maintenance and queues PAUSE. |
| `POST /api/agents/:id/resume` | operator, agent access | none | `{ok}`; leaves maintenance and queues RESUME. |
| `POST /api/agents/:id/restart_collectors` | operator, agent access | none | `{ok}`; queues RESTART_COLLECTORS. |
| `POST /api/agents/:id/disconnect` | operator, agent access | none | `{ok}`; queues DISCONNECT. |
| `POST /api/agents/:id/get_status` | operator, agent access | none | `{ok}`; queues an immediate report. |
| `POST /api/agents/:id/description` | operator, agent access | `{description}` | `{ok}`. |
| `POST /api/agents/:id/runbook` | operator, agent access | `{markdown}` | `{ok}`. |
| `GET /api/agents/:id/history` | viewer, agent access | `?limit=` | Array of status transitions `{id, agent_id, indicator, old_status, new_status, message, created_at}`. |
| `GET /api/agents/:id/log-matches` | viewer, agent access | `?limit=` | Array of `{match_id, agent_id, indicator_name, path, matched_line, severity, created_at}`. |
| `GET /api/uptime/:id` | viewer, agent access | `?days=` | `{agent_id, days, uptime_percent, actual_samples, expected_samples}`. |

## Metrics

| Route | Role | Request | Response and errors |
| --- | --- | --- | --- |
| `GET /api/metrics` | viewer | none | Array of `{agent_id, timestamp_ms, metrics}` for the latest sample of each visible agent. A row that cannot be decoded is logged and skipped. |
| `GET /api/metrics/:id` | viewer, agent access | `?limit=` | Same shape, newest first, undecodable rows skipped. |

## Alerts

| Route | Role | Request | Response and errors |
| --- | --- | --- | --- |
| `GET /api/alerts` | viewer | `?include_archived=1`, `?limit=`, `?offset=` | Array of alerts the caller can see: `{alert_id, agent_id, indicator, old_status, new_status, message, created_at, acknowledged_by, acknowledged_at, deleted_at, note, escalated_at, runbook_url, runbook_markdown}`. |
| `GET /api/alerts/unacknowledged` | viewer | `?limit=`, `?offset=` | Same shape, active and unacknowledged only. |
| `POST /api/alerts/:id/ack` | operator | optional `{note}` | `{ok}`. `404` unknown or archived alert, `403` alert's agent is outside the caller's groups. |
| `POST /api/alerts/bulk-ack` | operator | `{alert_ids, note?}` | `{ok, count}`. Ids that do not exist, are archived or are outside the caller's groups are skipped and not counted. `400` for a non-integer id or over 1000 ids. |
| `POST /api/alerts/bulk-archive` | operator | `{alert_ids}` | `{ok, count}`, with the same rules. |
| `DELETE /api/alerts/:id` | operator | none | `{ok}`; archives (soft-deletes) the alert. `404`, `403` as for ack. |

## Maintenance windows and silences

Group users can only create, list and delete entries for agents they can access.
Global roles may also use `"*"` (all agents).

| Route | Role | Request | Response and errors |
| --- | --- | --- | --- |
| `GET /api/maintenance-windows` | viewer | none | Array of `{window_id, agent_id, start_ms, end_ms, reason, created_by, created_at}`. |
| `POST /api/maintenance-windows` | operator | `{agent_id?, start_ms, end_ms, reason?}` (`agent_id` defaults to `*`) | `{window_id}`. `400` if `end_ms` is not after `start_ms`, `403` outside the caller's groups, `404` unknown agent. |
| `DELETE /api/maintenance-windows/:id` | operator | none | `{ok}`. `403` outside the caller's groups, `404`. |
| `GET /api/silences` | operator | none | Array of `{silence_id, agent_id, indicator, reason, until_ms, created_by, created_at}`. |
| `POST /api/silences` | operator | `{agent_id?, indicator?, reason?, until_ms}` (`agent_id` and `indicator` default to `*`) | `{silence_id}`. `400` if `until_ms` is not in the future, `403`, `404` unknown agent. |
| `DELETE /api/silences/:id` | operator | none | `{ok}`. `403`, `404`. |

## Views

A view is a named list of agents. Group users can only use views of groups they
belong to.

| Route | Role | Request | Response and errors |
| --- | --- | --- | --- |
| `GET /api/views` | viewer | none | Array of `{view_id, name, owner_user_id, owner_username, is_public, group_id, agent_ids, created_at}`. |
| `GET /api/views/:id` | viewer | none | One view. `403` outside the caller's groups, `404`. |
| `POST /api/views` | operator | `{name, is_public?, group_id?, agent_ids?}` | `201` with the view. `400` over a limit. |
| `PUT /api/views/:id` | operator; owner or global role | any of `name`, `is_public`, `group_id`, `agent_ids` | The updated view. `403`, `404`. |
| `DELETE /api/views/:id` | operator; owner or global role | none | `{ok}`. `403`, `404`. |

## Runbooks

| Route | Role | Request | Response and errors |
| --- | --- | --- | --- |
| `GET /api/runbooks` | viewer | none | Array of `{runbook_id, agent_id, indicator, status, url, markdown, notes, created_by, created_at}`; agent runbooks only for agents the caller can see. |
| `POST /api/runbooks` | global admin | `{status, url, agent_id?, indicator?, notes?}` (`status` is `yellow`, `amber` or `red`; `url` starts with `http://` or `https://`; `indicator` defaults to `*`) | `201` with the runbook. `404` unknown agent. |
| `DELETE /api/runbooks/:id` | global admin | none | `{ok}`. `404`. |

## Settings and reports

| Route | Role | Request | Response and errors |
| --- | --- | --- | --- |
| `GET /api/settings` | operator and global | none | `{webhook_url, offline_after_seconds, escalation_timeout_seconds, metrics_retention_days, reports_enabled, reports_schedule, reports_hour, reports_day_of_week, reports_webhook_url}`. A stored value that is not a number is reported as its default. |
| `PUT /api/settings` | operator and global | any of the fields above | `{ok}`. Ranges: `offline_after_seconds` 10 to 86400, `escalation_timeout_seconds` 60 to 604800, `metrics_retention_days` 1 to 365, `reports_schedule` `daily` or `weekly`, `reports_hour` 0 to 23, `reports_day_of_week` 0 to 6. |
| `POST /api/reports/send` | operator and global | none | `{sent, report}`; builds and sends the fleet digest now. |
