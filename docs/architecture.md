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
- Mark approved connected agents offline after `offline_after_seconds`.
- Evaluate submitted metrics, record status transitions, and create alerts only
  on worsening transitions.

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
common/signal_guard.hpp
common/stop_relay.hpp
common/zmq_retry.hpp
common/log_settings.hpp
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
