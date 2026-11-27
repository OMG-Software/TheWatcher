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

Schema migrations are additive and idempotent in both backends:
`SqliteStore::init_schema()` adds missing columns after a column check;
`PostgresStore::init_schema()` uses `ADD COLUMN IF NOT EXISTS` and
`current_schema()`, so the store lives in whichever schema the connection's
`search_path` selects (set it with `options=-csearch_path=myschema` in
`POSTGRES_DSN`; the default is `public`). The PostgreSQL constructor runs the
whole migration and the default-admin bootstrap in one transaction that holds
`pg_advisory_xact_lock`, so server nodes starting together queue behind one
another instead of racing, and a failed start leaves nothing half migrated.
The lock is transaction-scoped on purpose: it also works behind pgBouncer in
transaction pooling mode.

### SQLite and PostgreSQL behave the same

Both backends sit behind the `Store` interface and the same scenario list
(`server_tests/store_test.cpp`) runs against each. Where they used to differ,
the contract is now:

| Behaviour | Both backends |
| --- | --- |
| `get_metrics_in_window` | Newest `limit` rows (default 2,880) in `[since, until]`, oldest first. |
| Limit of 0 or less (`get_metrics`, `get_metrics_in_window`, `list_status_history`, `list_log_matches`) | Returns no rows. |
| Status history, metrics, alerts, agents, silences, log matches, views, runbooks, maintenance windows | Ordered by the documented column, then by id (or agent id), so ties are deterministic. |
| `list_groups`, `list_users` | Byte order of the name (PostgreSQL uses `COLLATE "C"`, not the database collation). |
| `latest_metrics` | One row per agent; the later insert wins a timestamp tie; sorted by agent id. |
| `create_session` with an existing token | Replaces that session. |
| `prune_metrics_before` | Deletes in batches of 5,000 rows, using `metrics(timestamp_ms)`. |

Differences that remain, by design: SQLite stores booleans as integers and
uses `AUTOINCREMENT` ids; PostgreSQL uses `BOOLEAN`/`BIGSERIAL`. SQLite is a
single file written by one process; PostgreSQL is shared by several server
nodes, which is why multi-statement writes and the heartbeat alert insert are
atomic on the database (not only under the in-process mutex).

### PostgreSQL connection recovery

`postgres::Connection` (`server/postgres_raii.hpp`) runs every statement
through `exec`/`exec_params`. A connection found dead before a statement is
reset (`PQreset`, three attempts, 100/200 ms apart) and the statement runs. If
the connection dies during a `SELECT`, the `SELECT` is retried once on the new
connection. A write that dies with the connection is not repeated, because it
may already have been applied: it throws, and the next call reconnects. A
connection lost inside a transaction is never reset mid-transaction; the call
throws, the guard rolls back, and the next call reconnects. After a pgBouncer
restart or failover the server therefore recovers by itself, with at most one
failed write per outage. `PostgresStore::set_statement_hook` is a test seam (unset in production) that lets the tests cut the connection while a statement or transaction is in flight. Add `connect_timeout=5` to `POSTGRES_DSN` so a
black-holed server does not stall a call for the OS default.

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
- Insert-if-absent across server nodes (`insert_heartbeat_alert_if_absent`)
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
