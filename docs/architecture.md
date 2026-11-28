# Architecture

TheWatcher has four primary runtime pieces: server, agent, common protocol, and
dashboard.

## Server

Files:

```text
server/main.cpp
server/server.cpp
server/api.cpp
server/store_sqlite.cpp
server/store_postgres.cpp
server/status_engine.cpp
server/alert_pipeline.cpp
server/offline_alerts.cpp
server/notification_worker.cpp
server/report_generator.cpp
server/zap_handler.cpp
server/config.cpp
```

Responsibilities:

- Load or create `TheWatcherServer.conf` (KEY=VALUE format).
- Bind the ZeroMQ ROUTER data socket on `bind_address`.
- Bind the ZeroMQ REP enrollment socket on `enrollment_address`.
- Start the HTTP REST API on `api_host:api_port`.
- Persist agents and metrics through `Store`: `SqliteStore` (default) or `PostgresStore` (`DB_TYPE=postgres`).
- Persist users, groups, sessions, status history, alerts, silences, views,
  runbooks, maintenance windows, log matches, and server settings.
- Persist per-agent CPU, memory, disk, and network status thresholds.
- Approve/reject/delete enrolled agents.
- Dispatch queued commands to agents over the existing agent connection.
- Respond to agent config refresh requests.
- Mark approved connected agents offline after their effective offline
  timeout (see Server Maintenance Tick).
- Evaluate submitted metrics, record status transitions, and create alerts only
  on worsening transitions.
- Raise every alert through one path, `AlertPipeline` (see
  [Alerts and notifications](#alerts-and-notifications)).
- Deliver alert webhooks and scheduled report digests on a separate worker
  thread, never on the data-plane thread.

The server does not initiate network connections to agents.

### Status Engine Lifecycle

The server owns one long-lived `StatusEngine`. Its per-agent state (stale-value
clocks and the anomaly baseline cache, keyed by agent and indicator) therefore
survives from one metrics frame to the next. The engine is only used from the
server's run loop thread. It is built with the server's `AlertPipeline` and
so is destroyed before it. `evaluate_metrics` no longer prunes anything.

Status rules:

- Grey means "no data" (for example a NaN or infinite reading). A transition to
  Grey never raises an alert.
- Blue means maintenance. A transition to Blue never raises an alert.
- Grey and Blue carry no severity when they are the previous state: leaving
  either for Yellow, Amber or Red is a worsening and alerts (after the
  configured consecutive readings), leaving them for Green does not.

### Maintenance State Machine

Manual, timed and scheduled maintenance share one state machine in
`StatusEngine`:

- `enter_maintenance(agent, reason, until_ms, now)` sets the flag, clears the
  agent's active alerts and marks the agent's real indicators Blue: `cpu`,
  `memory` and every indicator already in its status history (`disk:<mount>`,
  `network:<iface>`, `process:<name>`, ...).
- `exit_maintenance(agent)` clears the flag. Indicators stay Blue until the
  next metrics frame replaces them; the first real reading after maintenance
  alerts normally even if it is already Red. Entering and leaving maintenance
  also resets the agent's stale-value clocks and baselines, so readings frozen
  during maintenance are not flagged stale afterwards.
- Timed manual maintenance (`until_ms > 0`) ends when `now >= until_ms`.
  `until_ms = 0` is open ended and ends only on resume. If a maintenance
  window is active when it expires, the agent moves straight into the window's
  maintenance in the same tick.
- A PAUSE command acknowledged by the agent keeps the reason and expiry the API
  already stored; only an agent not yet in maintenance gets the open-ended
  reason `command`.
- A scheduled maintenance window covering an agent (`agent_id` or `*`) starts
  maintenance with the reason `mw:<window reason>` and ends it when the window
  is over. Only maintenance whose reason starts with `mw:` is ended by a
  window; manual maintenance is never ended by the absence of a window.
- Maintenance set by the API (`POST /api/agents/:id/maintenance`, pause) only
  sets the flag; the next tick marks the indicators Blue.
- "In maintenance" means the agent's stored maintenance flag, for manual,
  timed and scheduled maintenance alike. The state machine is the only writer
  of the flag for timed and scheduled maintenance, and both `StatusEngine`
  (it skips metric evaluation) and `AlertPipeline` (it suppresses every alert
  kind) read that same flag, so they agree at a window boundary: the tick that
  ends a window clears the flag, and alerts are raised again from then on.

### Server Maintenance Tick

`Server::run_periodic_maintenance` runs about once a second from the run loop,
independent of any agent traffic. It:

1. reconciles maintenance (above);
2. runs the retention job at most once an hour (`run_retention`).

The once-a-second offline scan sits next to it. The rule lives in
`OfflineAlerts` (`server/offline_alerts.cpp`), which marks silent agents
offline and then raises their `Heartbeat` alerts through `AlertPipeline`:

- Base timeout: the `offline_after_seconds` server setting (dashboard Settings,
  10-86400), falling back to the `OFFLINE_AFTER_SECONDS` config value (default
  60) when the setting was never saved. A non-positive config value disables
  offline detection.
- Effective timeout per agent: `max(base, 3 x heartbeat_interval)`. With the
  default 5 s heartbeat the base applies; a 60 s heartbeat gets 180 s, so an
  agent heartbeating on time never flaps offline.
- The saved setting overrides the config value in both directions: a saved
  value of 0 or less disables offline detection, and a saved positive value
  enables it even when `OFFLINE_AFTER_SECONDS` is 0 in the config.
- The same per-agent timeout expires an agent's learned command route when it
  has sent no frame for that long.
- Agents in maintenance are never marked offline.
- An agent an operator disconnected on purpose (#156) is marked offline but
  raises no alert until it sends a frame again.
- A newly offline agent gets one open `Heartbeat` alert, archived when it
  reconnects.

### Data Retention

`run_retention` prunes, per run (hourly, and once at server start):

| Data | Setting (days) | Default | Rule |
| --- | --- | --- | --- |
| Metrics | `metrics_retention_days` | 30 | older rows are deleted |
| Status history | `status_history_retention_days` | 90 | older rows are deleted, except the newest row of every agent/indicator (it is the current state) |
| Alerts | `alerts_retention_days` | 90 | older alerts are deleted only once acknowledged or soft-deleted; open alerts are never pruned |
| Log matches | `log_matches_retention_days` | 30 | older rows are deleted |
| Silences, maintenance windows | `alerts_retention_days` | 90 | deleted once expired / ended for longer than the period |

Invalid or non-positive values fall back to the default. Sessions are not
pruned by this job. The tick also drops the engine's cached state (stale-value
clocks, baselines) of agents that were deleted.

## Agent

Files:

```text
agent/main.cpp
agent/config.cpp
agent/enrollment.cpp
agent/agent.cpp
agent/collectors/*
```

Responsibilities:

- Load or create `TheWatcherAgent.conf`.
- Generate a stable agent id and CURVE keypair on first run.
- Submit enrollment requests until approved or rejected.
- Learn the approved server public key, persist its pinned fingerprint, and
  reject later approvals from a different server key.
- Establish a ZeroMQ DEALER connection to the server data endpoint.
- Collect metrics periodically and submit `METRICS` frames.
- Send `HEARTBEAT` frames.
- Send `CONFIG_REQUEST` after each metrics submission.
- Apply `CONFIG_UPDATE` responses.
- Execute server commands such as pause, resume, restart collectors, status, and
  disconnect.

The agent is always the side that establishes connections.

## Common Protocol

Files:

```text
common/protocol.hpp
common/commands.hpp
common/metrics.hpp
common/crypto.hpp
common/logging.hpp
common/windows_service.hpp
```

Protocol frames are CBOR-encoded `thewatcher::proto::Frame` values (libcbor
0.13.0). Each frame is a definite-length CBOR array of `[type, agent_id,
timestamp_ms, payload]`. The payload is itself a CBOR-encoded buffer whose
shape is determined by `FrameType`.

Frame types:

| Frame | Direction | Purpose |
| --- | --- | --- |
| `HEARTBEAT` | Agent to server | Updates `last_seen`. |
| `METRICS` | Agent to server | Sends a structured `SystemMetrics` snapshot. |
| `COMMAND` | Server to agent | Queued operator command. |
| `COMMAND_ACK` | Agent to server | Command result. |
| `CONFIG_UPDATE` | Server to agent | Runtime settings response. |
| `ENROLL_REQUEST` | Agent to server | Enrollment request on the enrollment socket. |
| `ENROLL_RESPONSE` | Server to agent | Pending, approved, or rejected enrollment state. Approved responses include the server public key and BLAKE2b-256 fingerprint. |
| `CONFIG_REQUEST` | Agent to server | Requests current runtime settings after metrics submission. |

Commands are defined in `common/commands.hpp` and CBOR-serialised. Metrics are
defined in `common/metrics.hpp`, CBOR-serialised on the wire, persisted as a
CBOR BLOB in the store, and converted to JSON inside `server/api.cpp` (via
`server/api_json.hpp`'s nlohmann adapters) when served to the dashboard.

## Storage

Files:

```text
server/store.hpp
server/store_sqlite.cpp
server/store_sqlite.hpp
server/store_postgres.cpp
server/store_postgres.hpp
```

`Store` is the server storage boundary. `SqliteStore` and `PostgresStore`
(the latter only compiled when PostgreSQL support is enabled at build time;
see [Build Environment](build-environment.md)) both persist:

- agent identity and enrollment state;
- approved/rejected flags;
- connected and maintenance state;
- maintenance reason and optional expiry;
- collection interval and process limit;
- agent and user group membership;
- users, roles, password hashes, and sessions;
- status history and soft-deletable alerts;
- runtime server settings such as thresholds and webhook URL;
- first seen and last seen timestamps;
- metric snapshots;
- silences, saved views, runbooks, maintenance windows, and matched log lines.

Schema migrations are implemented as additive `ALTER TABLE` calls guarded by
column checks in `init_schema()` of each store.

### Store threading and transactions

Files: `server/store_synchronized.hpp`, `server/sqlite_raii.hpp`,
`server/postgres_raii.hpp`.

- One database connection per store, and a connection is used by one thread
  at a time. `make_store()` wraps the backend in `SynchronizedStore`, which
  holds a single mutex for the whole of every public call. A transaction
  lives inside one call, so no other call can run inside it. The wrapper
  implements the full `Store` interface: a new virtual method that is not
  forwarded there does not compile, so the lock cannot be forgotten.
  Backends never call back into the wrapper. Tests that construct
  `SqliteStore` or `PostgresStore` directly are single-threaded.
- A method that writes more than one row or statement is atomic. Use the
  RAII guard (`sqlite::Transaction`, `postgres::Transaction`): it begins on
  construction, rolls back on destruction unless `commit()` succeeded, and
  never throws from the destructor, so the original error is kept. Do not
  start a transaction inside another one: share a private helper that does
  not begin its own (see `replace_agent_groups`).
- Insert-if-absent across server nodes (`insert_heartbeat_alert_if_absent`,
  called by `AlertPipeline` for requests with `single_open_heartbeat`, which
  skips the webhook when it returns 0)
  cannot rely on the in-process mutex. On PostgreSQL the check and the insert
  run in one transaction holding
  `pg_advisory_xact_lock(hashtext('thewatcher:heartbeat:<agent_id>'))`,
  which serialises them per agent across every node. The lock only protects
  nodes that take it: a node older than 0.9.0 still scans then inserts
  without it, so upgrade every node before running a mixed cluster
  concurrently, or accept a possible duplicate Heartbeat alert during the
  upgrade. No unique index is used, so existing databases that already hold
  duplicates need no migration.
- Read paths throw on a database error. `sqlite::step_row` returns false only
  for `SQLITE_DONE`; "no rows" is never the result of a busy or failing
  statement. SQLite waits up to 5 seconds for a lock (`busy_timeout`).
- Never rewrite a whole agent row from a snapshot. Use the narrow setters
  (`touch_agent`, `set_agent_collection_interval`,
  `set_agent_heartbeat_interval`, `set_agent_process_limit`,
  `set_agent_collector_config`, `set_agent_runbook`, `set_agent_maintenance`,
  `set_agent_thresholds`, `set_agent_description`) or, for the configure
  modal, `save_agent_configuration`, which applies every field and the group
  memberships in one transaction. `upsert_agent` is for enrollment only.
- Database handles and statements are owned by RAII types
  (`sqlite::Connection`, `sqlite::Stmt`, `postgres::Connection`).

## REST API

Implemented in `server/api.cpp`.

Important endpoints:

| Endpoint | Purpose |
| --- | --- |
| `POST /api/login` | Create a SQLite-backed authenticated session. |
| `POST /api/logout` | Delete the current session. |
| `GET /api/session` | Return the current session. |
| `GET /api/agents` | List approved agents and runtime state. |
| `GET /api/pending-enrollments` | List pending enrollment records. |
| `POST /api/agents/:id/approve` | Approve enrollment, assign groups, and add the agent key to ZAP. |
| `POST /api/agents/:id/reject` | Reject enrollment and remove the key from ZAP. |
| `POST /api/agents/:id/groups` | Replace an approved agent's group memberships. |
| `DELETE /api/agents/:id` | Delete the agent and its metrics. |
| `GET /api/groups` | List groups. |
| `POST /api/groups` | Create a group. |
| `GET /api/users` | List users. |
| `POST /api/users` | Create a user with role and groups. |
| `GET /api/alerts` | List active alerts. |
| `GET /api/alerts/unacknowledged` | List active unacknowledged alerts. |
| `POST /api/alerts/:id/ack` | Acknowledge an alert. |
| `DELETE /api/alerts/:id` | Soft-delete an alert. |
| `GET /api/metrics` | Latest metrics row per agent. |
| `GET /api/metrics/:id?limit=N` | Metric history for one agent. |
| `POST /api/agents/:id/set_interval` | Persist and queue interval update. |
| `POST /api/agents/:id/set_process_limit` | Persist and queue process limit update. |
| `POST /api/agents/:id/thresholds` | Persist CPU, memory, disk, and network thresholds for one agent. |
| `POST /api/agents/:id/maintenance` | Enter maintenance with reason and expiry metadata. |
| `POST /api/agents/:id/pause` | Queue pause command. |
| `POST /api/agents/:id/resume` | Queue resume command. |
| `POST /api/agents/:id/restart_collectors` | Queue collector restart. |
| `POST /api/agents/:id/disconnect` | Queue graceful disconnect. |
| `POST /api/agents/:id/get_status` | Queue immediate metrics report. |

## Dashboard

Files:

```text
dashboard/src/api.ts
dashboard/src/status.ts
dashboard/src/main.tsx
dashboard/src/App.tsx
dashboard/src/pages/
dashboard/src/components/
dashboard/src/hooks/
dashboard/src/permissions.ts
dashboard/src/models.ts
dashboard/src/styles.css
```

The dashboard is a React + TypeScript + Vite app. It authenticates with the
server API, polls approved agents, metrics, pending enrollments, groups, users,
and alerts, maps backend rows into dashboard view models, and provides operator
controls for runtime settings, commands, maintenance, alert handling, and
deletion.

## Alerts and Notifications

Every alert kind (metric transitions from `StatusEngine`, the offline-agent
`Heartbeat` alert, and `LOG_MATCH` alerts) is raised through
`AlertPipeline::raise` (`server/alert_pipeline.cpp`). In order it:

1. rejects a severity other than `yellow`, `amber` or `red`;
2. skips the alert while the agent is in maintenance;
3. skips it when a silence matches the agent and indicator;
4. optionally de-duplicates: log-match and offline alerts are skipped while an
   unacknowledged alert with the same agent, indicator and severity is open,
   so 100 matching log lines leave one open alert (metric alerts are already
   limited to worsening transitions);
5. attaches the matching runbook URL, stores the alert and queues the
   `notifications.webhook_url` webhook.

A new alert source builds an `AlertRequest` and calls `raise`; behaviour that
applies to every alert (for example another notification channel or extra
alert fields) is added in that one function.

### Delivery worker

`NotificationWorker` (`server/notification_worker.cpp`) owns a `std::jthread`
and a bounded queue (256 entries). The data plane only validates the URL
(`parse_webhook_url`, unchanged), builds the JSON payload and calls `submit`,
which never blocks: if the queue is full the notification is dropped and
logged. The worker does the HTTP POST with 2 s (alerts) or 5 s (reports)
connect, read and write timeouts, treats only a 2xx response as delivered, and
retries a failed alert webhook up to 3 times with a 1 s, then 2 s, backoff. It
never reads or writes the store; results go back to the server loop, which
records them. Shutdown stops the thread, abandons queued notifications and
joins once the request in flight ends.

### Scheduled reports

When `reports.enabled` is `true`, the server loop queues a digest to
`reports.webhook_url` once per period, inside the scheduled UTC hour:

| Setting | Default | Meaning |
| --- | --- | --- |
| `reports.enabled` | `false` | `true` turns the scheduled digest on. |
| `reports.webhook_url` | empty | Destination; validated like the alert webhook. |
| `reports.schedule` | `daily` | `daily` or `weekly`. |
| `reports.hour` | `8` | UTC hour of day, 0-23. |
| `reports.day_of_week` | `1` | Weekly only: 0 = Sunday ... 6 = Saturday. |
| `reports.last_sent_ms` | `0` | Written by the server after a 2xx response; read at every tick, so a restart in the scheduled hour does not send a second digest. |

A malformed or out-of-range value falls back to its default. A digest with no
response or a non-2xx status is a failure: the last-sent time is not updated
and it is retried after 1, 2, 4, 8 and then 15 minutes while the hour lasts.
`POST /api/reports/send` still sends immediately and synchronously on the API
thread and returns `sent: true` only for a 2xx response.

### Offline agents and operator disconnects

The offline scan (once a second) raises a red `Heartbeat` alert for an approved
agent that is disconnected, not in maintenance and has none open. An operator
`DISCONNECT` is deliberate and does **not** alert: when the agent ACKs it, the
server marks the agent operator-disconnected (persisted in the
`agents.operator_disconnected` setting, so it survives a restart) and the scan
skips it. The mark is cleared when the agent next sends any frame, after which
an unexpected loss alerts as usual. The agent is shown as disconnected, not
offline-red, and nothing escalates.

### Commands for disconnected agents

The server cannot reach an agent that is not connected, and commands are not
queued for later. Every route that queues a command (`pause`, `resume`,
`restart_collectors`, `disconnect`, `get_status`, `command`, `set_interval`,
`set_process_limit`) first calls one admission check and answers `409` with
`agent is offline; command not queued` when the agent cannot receive it. The
check changes nothing: no setting is stored and nothing is queued. "Can
receive" means the server holds a live route for the agent (it has sent an
authenticated frame since the server started and the route has not expired),
not just the persisted `connected` flag, which survives a restart. `POST
/api/agents/:id/maintenance` still sets maintenance on the server and sends the
`PAUSE` command only to a reachable agent.

The ROUTER socket uses `ZMQ_ROUTER_MANDATORY` and every part of a command is
sent with `dontwait`. A peer whose connection has just closed (`EHOSTUNREACH`)
or a full send queue makes the command undelivered: it is logged and not
recorded as dispatched.

`GET /api/settings` returns the effective `reports_schedule`, `reports_hour`
and `reports_day_of_week`; a malformed stored value reads as its default.
`reports.last_sent_ms` must be one whole non-negative number, otherwise it is
treated as never sent.

## Data Flow

```text
Agent config -> enrollment REQ -> Server enrollment REP with key fingerprint
Agent DEALER -> Server ROUTER
Agent METRICS/HEARTBEAT/CONFIG_REQUEST -> Server
Server CONFIG_UPDATE/COMMAND -> Agent
Agent COMMAND_ACK -> Server
Server REST API -> Dashboard
```

The dashboard talks only to the server API. It never talks directly to agents.
