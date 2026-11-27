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
- Persist agents and metrics through `Store`, currently `SqliteStore`.
- Persist users, groups, sessions, status history, alerts, and server settings.
- Persist per-agent CPU, memory, disk, and network status thresholds.
- Approve/reject/delete enrolled agents.
- Dispatch queued commands to agents over the existing agent connection.
- Respond to agent config refresh requests.
- Mark approved connected agents offline after `offline_after_seconds`.
- Evaluate submitted metrics, record status transitions, and create alerts only
  on worsening transitions.
- Raise every alert through one path, `AlertPipeline` (see
  [Alerts and notifications](#alerts-and-notifications)).
- Deliver alert webhooks and scheduled report digests on a separate worker
  thread, never on the data-plane thread.

The server does not initiate network connections to agents.

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
CBOR BLOB in SQLite, and converted to JSON inside `server/api.cpp` (via
`server/api_json.hpp`'s nlohmann adapters) when served to the dashboard.

## Storage

Files:

```text
server/store.hpp
server/store_sqlite.cpp
server/store_sqlite.hpp
```

`Store` is the server storage boundary. `SqliteStore` persists:

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
- metric snapshots.

Schema migrations are implemented as additive `ALTER TABLE` calls guarded by
column checks in `SqliteStore::init_schema()`.

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
