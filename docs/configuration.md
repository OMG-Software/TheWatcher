# Configuration

This document lists the server and agent config files, default paths, command
line overrides, and every supported option.

## Server Config

The server config is `KEY=VALUE` text (one option per line, `#` for comments).

Default paths:

```text
Windows: C:\ProgramData\TheWatcher\TheWatcherServer.conf
Linux/BSD: /etc/thewatcher/TheWatcherServer.conf
```

The server uses `--config <path>` when provided. Otherwise it uses the platform
default path. If the file does not exist, the server creates it and generates a
CURVE keypair. Existing config files above 1 MiB, or with any single line above
8 KiB, are rejected before settings are applied.

Example:

```text
BIND_ADDRESS=tcp://*:5555
ENROLLMENT_ADDRESS=tcp://*:5556
API_HOST=0.0.0.0
API_PORT=8080
DB_PATH=thewatcher.db
DB_TYPE=sqlite
POSTGRES_DSN=
OFFLINE_AFTER_SECONDS=60
SERVER_PUBLIC_KEY=<40-character-z85-public-key>
SERVER_SECRET_KEY=<40-character-z85-secret-key>
```

Options:

| Option | Default | Description |
| --- | --- | --- |
| `BIND_ADDRESS` | `tcp://*:5555` | ZeroMQ ROUTER endpoint for approved agents. Agents connect to this endpoint and submit metrics, heartbeats, command ACKs, and config requests. |
| `ENROLLMENT_ADDRESS` | `tcp://*:5556` | ZeroMQ REP endpoint for plaintext enrollment requests. |
| `API_HOST` | `0.0.0.0` | HTTP REST API bind host. |
| `API_PORT` | `8080` | HTTP REST API port consumed by the dashboard. |
| `DB_PATH` | `thewatcher.db` | SQLite database path when `DB_TYPE` is `sqlite`. Relative paths resolve beside the active server config file so server restarts from different working directories keep using the same database. |
| `DB_TYPE` | `sqlite` | Storage backend. `sqlite` is implemented. `postgres` is reserved in config but not implemented by the current build. |
| `POSTGRES_DSN` | empty | Reserved for future PostgreSQL support. |
| `OFFLINE_AFTER_SECONDS` | `60` | Approved connected agents are marked disconnected when `last_seen` is older than this number of seconds. Set `0` or a negative value to disable automatic offline marking. |
| `SERVER_PUBLIC_KEY` | generated | Server CURVE public key. Approved enrollment responses include this key and its fingerprint so agents can learn and pin it. |
| `SERVER_SECRET_KEY` | generated | Server CURVE secret key. Keep this private. |
| `ENROLLMENT_TOKEN` | empty | Optional pre-shared token. When set, every enrollment request must carry a matching token (`ENROLLMENT_TOKEN` in the agent config), so arbitrary hosts cannot create pending-agent records. Leave empty for tokenless first-run enrollment. |
| `TRUSTED_PROXIES` | empty | Comma-separated list of reverse-proxy addresses whose `X-Forwarded-For` / `X-Real-IP` headers the API trusts when attributing login attempts for throttling. Whitespace around entries is ignored and empty entries are skipped. Empty (the default) trusts no proxy, so those headers are ignored and the TCP peer address is used. Omitted from the saved file when empty. |

If `server_public_key` is empty, the server does not enable CURVE on the data
socket. The default generated config enables CURVE.

When the server writes its config (first run, or after a change), it writes to
a uniquely named temporary file in the same directory that is created
owner-only (`0600` on POSIX) and then renames it over the config, so an
interrupted write never truncates the previous config and the secret key is
never readable by other users. A missing config directory is created
owner-only (`0700`); a directory that already exists is left unchanged. On
Windows the temporary file is created exclusively and swapped in with
`MoveFileExW(MOVEFILE_REPLACE_EXISTING)`, and access is governed by the
directory ACL set by the installer.

## Server Database Settings

The server also stores runtime settings in SQLite table `server_settings`.
These values are changed through API or direct administrative DB updates until
dedicated dashboard controls exist.

| Key | Default | Description |
| --- | --- | --- |
| `notifications.webhook_url` | empty | Optional HTTP webhook URL. The server posts alert JSON when an alert is generated. `http://` URLs are supported by the current build. Localhost, private, link-local, multicast, documentation, benchmark, malformed authority, userinfo, and invalid-port targets are rejected before sending. |

## Per-Agent Collector Configuration

Approved agents have a persisted `collector_config` JSON document. The Agents
page exposes it through the Configure button beside the agent name. The
dashboard sends the whole config to:

```text
POST /api/agents/:id/collector_config
```

Payload shape:

```json
{
    "collection_interval": 30,
    "heartbeat_interval": 5,
    "process_limit": 25,
    "group_ids": [1],
    "runbook_markdown": "# Host runbook\n\nCheck service state first.",
    "collector_config": {
    "cpu": { "warning_percent": 80, "degraded_percent": 90, "critical_percent": 95 },
    "memory": { "warning_percent": 80, "degraded_percent": 90, "critical_percent": 95 },
    "cpu_readings": 1,
    "memory_readings": 1,
    "disk_readings": 1,
    "network_readings": 1,
    "process_readings": 3,
    "disks": [
      {
        "mount_point": "/data",
        "device": "/dev/sdb1",
        "enabled": true,
        "thresholds": { "warning_percent": 80, "degraded_percent": 90, "critical_percent": 95 }
      }
    ],
    "networks": [
      {
        "interface_name": "eth0",
        "enabled": true,
        "thresholds": { "warning_mbps": 100, "degraded_mbps": 200, "critical_mbps": 300 }
      }
    ],
    "processes": [
      { "name": "TheWatcherAgent.exe", "expected_count": 1, "enabled": true }
    ]
  }
}
```

Collector fields:

| Field | Default | Description |
| --- | --- | --- |
| `cpu` | 80/90/95 percent | Absolute CPU usage thresholds. |
| `memory` | 80/90/95 percent | Absolute memory used thresholds. |
| `cpu_readings` | `1` | Consecutive worsening CPU readings required before the worse state is committed. |
| `memory_readings` | `1` | Consecutive worsening memory readings required before the worse state is committed. |
| `disk_readings` | `1` | Consecutive worsening disk readings required before the worse state is committed. |
| `network_readings` | `1` | Consecutive worsening network readings required before the worse state is committed. |
| `process_readings` | `3` | Consecutive missing process readings required to reach red. The first failed reading is yellow, the second is amber, and the third is red when the default is used. |
| `disks` | empty | Per fixed-disk configuration. An empty list means monitor all reported fixed disks with default thresholds. Entries are matched by `mount_point` and displayed as `mount point (device)`. |
| `networks` | empty | Per-interface configuration. An empty list means monitor all reported non-loopback interfaces. On Windows, reported interfaces are limited to IP Helper adapters that match `ipconfig` output; lower-layer filter, bridge, tunnel, and scheduler devices are ignored. Thresholds are combined receive plus transmit megabits per second. |
| `processes` | empty | Exact executable names and expected instance counts. Fewer running instances than `expected_count` escalates process health and the alert message names the missing process. |

`heartbeat_interval` is the per-agent heartbeat cadence in seconds. It is
validated independently from `collection_interval`; valid values are 1 through
60, and the default is 5. `group_ids` is accepted only from global roles and
replaces the approved agent's group membership. `runbook_markdown` stores the
agent-specific markdown runbook edited from the Configure modal and rendered in
the Alerts page Runbook modal.

Thresholds must be ordered `warning < degraded < critical`. Percent thresholds
must be no higher than 100 for the critical level. Consecutive-reading counters
reset when the collector recovers or when a different worse state is observed.

Status colors:

| Color | Meaning |
| --- | --- |
| Green | Healthy |
| Yellow | Warning |
| Amber | Degraded |
| Red | Critical or failure |
| Grey | No data |
| Blue | Maintenance |

## Authentication And RBAC

The first SQLite database initialization creates:

```text
username: thewatcher
password: look_at_me
role: global_admin
group: Admins
```

Passwords are stored as libsodium password hashes, not plaintext. Sessions are
stored in SQLite and returned to the browser as HTTP-only `tw_session` cookies.
Unsafe authenticated API requests that include an `Origin` or `Referer` header
must match the API scheme, host, and effective port, or come from a loopback
dashboard origin talking to a loopback API. `X-Forwarded-Proto` is used when the
API is behind an HTTPS reverse proxy. Other cross-origin browser writes are
rejected with HTTP 403. Allowed dashboard origins receive credentialed CORS
response headers so local dashboard/API port splits work without weakening
arbitrary cross-site writes. API responses also send baseline browser hardening
headers including `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY`,
`Cache-Control: no-store`, and a restrictive Content Security Policy.

Account types:

| Type | Permissions |
| --- | --- |
| `global_admin` | Full unrestricted access to all groups, agents, views, users, settings, and commands. |
| `global_operator` | Same operational access as `global_admin`, but users and groups are read-only. |
| `global_viewer` | Read-only access to all groups, agents, views, users, alerts, and metrics. |
| `group_admin` | Can see and configure agents, views, alerts, maintenance, and runbooks assigned to their groups, and can create users inside their own groups. |
| `group_operator` | Same group-scoped operational access as `group_admin`, but users are read-only. |
| `group_viewer` | Read-only access to agents, views, alerts, metrics, and users visible through their groups. |

Users can belong to multiple groups. Global account types can list every group
and group-scoped record. Group account types only receive their assigned groups
from list APIs and cannot see other groups in dashboard drop-down lists, views,
maintenance windows, or silence rules. Agents are assigned to groups on
approval and global users can replace an approved agent's group membership from
the agent Configure modal. Moving an agent to a different group also moves
alert visibility to that new group. Group operators can create maintenance
windows and silences only for agents inside their groups; global wildcard
maintenance windows and silences are global-account-only operations.

## Agent Config

The agent config is `KEY=VALUE` text (one option per line, `#` for comments).
JSON agent configs are no longer supported as of 0.3.0; delete any old
`agent.json` and let the agent create `TheWatcherAgent.conf` on first run.

Default paths:

```text
Windows foreground user: %APPDATA%\TheWatcher\TheWatcherAgent.conf
Windows recommended service path: C:\ProgramData\TheWatcher\TheWatcherAgent.conf
Linux/BSD: ~/.config/thewatcher/TheWatcherAgent.conf
```

Use `--config <path>` for services and repeatable deployments. If the file does
not exist, the agent creates it and generates an agent id plus CURVE keypair.
Existing config files above 1 MiB, or with any single line above 8 KiB, are
rejected before settings are applied.

Minimal example:

```text
THEWATCHER_SERVER=monitor.example.internal
```

Full example:

```text
THEWATCHER_SERVER=monitor.example.internal
SERVER_ADDRESS=tcp://monitor.example.internal:5555
ENROLLMENT_ADDRESS=tcp://monitor.example.internal:5556
SERVER_PUBLIC_KEY=<40-character-z85-server-public-key>
SERVER_PUBLIC_KEY_FINGERPRINT=<64-character-hex-fingerprint>
AGENT_ID=<generated-agent-id>
AGENT_PUBLIC_KEY=<40-character-z85-agent-public-key>
AGENT_SECRET_KEY=<40-character-z85-agent-secret-key>
COLLECTION_INTERVAL=30
HEARTBEAT_INTERVAL=5
PROCESS_LIMIT=25
```

Options:

| Option | Default | Description |
| --- | --- | --- |
| `THEWATCHER_SERVER` | `127.0.0.1` derived defaults | Hostname or IP of the server. The agent derives `tcp://<host>:5555` for data and `tcp://<host>:5556` for enrollment. |
| `SERVER_ADDRESS` | `tcp://127.0.0.1:5555` | Explicit data endpoint. Overrides the endpoint derived from `THEWATCHER_SERVER`. |
| `ENROLLMENT_ADDRESS` | `tcp://127.0.0.1:5556` | Explicit enrollment endpoint. Overrides the endpoint derived from `THEWATCHER_SERVER`. |
| `SERVER_PUBLIC_KEY` | empty | Server CURVE public key. Normal first enrollment learns this from the approved enrollment response. Set it manually only when pre-pinning a known server key or using `--server-key`. Once set it is a pin: an enrollment reply presenting a different key is rejected. |
| `SERVER_PUBLIC_KEY_FINGERPRINT` | empty | BLAKE2b-256 hex fingerprint of `SERVER_PUBLIC_KEY`. The agent writes this after approval and rejects future approved enrollment responses whose fingerprint does not match. |
| `ALLOW_INSECURE` | `0` | Explicit opt-in to connect without CURVE. When `0` (default), an approved enrollment reply without a server key pin is a hard error — the agent never silently runs an unencrypted data plane. |
| `ENROLLMENT_TOKEN` | empty | Pre-shared enrollment token matching the server's `ENROLLMENT_TOKEN`. Required when the server configures one. |
| `AGENT_ID` | generated UUID | Stable agent identity used for enrollment, routing, and dashboard rows. Valid characters are letters, digits, `.`, `_`, and `-` (max 64). |
| `AGENT_PUBLIC_KEY` | generated | Agent CURVE public key sent during enrollment and approved by the server. |
| `AGENT_SECRET_KEY` | generated | Agent CURVE secret key. Keep this private. |
| `COLLECTION_INTERVAL` | `30` | Seconds between periodic metrics submissions. The server can update this through the dashboard. |
| `HEARTBEAT_INTERVAL` | `5` | Seconds between heartbeat frames. The server can update this through the dashboard; valid values are 1 through 60. |
| `PROCESS_LIMIT` | `25` | Number of top processes collected by the process collector. The server can update this through the dashboard. |

The log monitor additionally respects the `LOG_ALLOWED_DIRS` environment
variable: a list of directories (separated by `;` on Windows, `:` elsewhere)
inside which the configured log paths must resolve. While unset, log
monitoring is disabled entirely — the agent fails closed rather than reading
arbitrary files chosen on the dashboard.

## Agent CLI Overrides

Agent command-line options override loaded config values for that process:

```text
--config <path>
--server <tcp-endpoint>
--enrollment <tcp-endpoint>
--server-key <40-character-z85-key>
--insecure
--service
--service-name <name>
--install-service
--uninstall-service
--genkey
--help
```

`--genkey` prints a new CURVE keypair and exits. `--insecure` is the CLI
equivalent of `ALLOW_INSECURE=1` and must be set explicitly to allow an
enrollment reply without a server key pin.

## Enrollment Key Pinning

Approved enrollment responses include:

```text
server_public_key_z85
server_public_key_fingerprint
```

The fingerprint is a BLAKE2b-256 hex digest of the Z85 server public key. On
first approval, the agent stores both values in `TheWatcherAgent.conf` before
opening the encrypted data socket. On later approvals, an existing
`SERVER_PUBLIC_KEY_FINGERPRINT` is treated as a pinned identity; if the server
returns a different fingerprint, enrollment fails instead of silently trusting a
new key. A configured `SERVER_PUBLIC_KEY` is likewise a pin: an approved reply
presenting a different key is rejected. An approved reply with no key at all
fails enrollment unless the agent runs with `--insecure` / `ALLOW_INSECURE=1`.

## Server CLI Options

```text
--config <path>
--service
--service-name <name>
--install-service
--uninstall-service
--genkey
--help
```

`--genkey` prints a new CURVE keypair and exits.

## Runtime Settings Flow

The dashboard writes per-agent runtime settings through the server API. The
server persists `collection_interval`, `heartbeat_interval`, `process_limit`, and `collector_config`
in the agents table. After every metrics submission, the agent sends a
`CONFIG_REQUEST`; the server responds with `CONFIG_UPDATE` containing the
current persisted settings. Agents apply the heartbeat cadence, process watches,
and collector settings from that config so watched executables are reported even
when they are outside the top process sample.

Agents establish all ZeroMQ connections. The server never dials agents.
