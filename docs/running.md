# Running TheWatcher

This document describes how to start, stop, and verify the server, agent, and
dashboard.

## Start The Server In Foreground Mode

```powershell
.\builddir-release\server\TheWatcherServer.exe --config C:\ProgramData\TheWatcher\server.json
```

Expected log lines include:

```text
Config: C:\ProgramData\TheWatcher\server.json
Log: C:\ProgramData\TheWatcher\TheWatcherServer.log
Public key: <server-public-key>
Running. Press Ctrl+C to stop.
```

Stop foreground mode with `Ctrl+C`.

## Start The Agent In Foreground Mode

Create or edit the agent config first:

```text
THEWATCHER_SERVER=127.0.0.1
```

Start:

```powershell
.\builddir-release\agent\TheWatcherAgent.exe --config C:\ProgramData\TheWatcher\TheWatcherAgent.conf
```

The agent enrolls first. Until an operator approves it, enrollment remains
pending and the agent keeps retrying.

Stop foreground mode with `Ctrl+C`.

## Signals And Exit Status

On Linux and the BSDs, `SIGINT` and `SIGTERM` are blocked in every thread and
picked up by one watcher thread (`common/signal_guard.hpp`), so a signal can
never interrupt a ZeroMQ call on a worker thread. Interrupted ZeroMQ calls
caused by anything else (for example `SIGSTOP`/`SIGCONT`) are retried
(`common/zmq_retry.hpp`). Both binaries exit `0` after a signal and `1` on a
startup or fatal runtime failure. A Windows service stop that arrives while the
server or agent is still starting is remembered and applied as soon as it is
ready (`common/stop_relay.hpp`).

The server keeps running when one loop iteration fails, for example a database
error or a corrupt stored setting: the error is logged and the iteration is
retried with a growing pause. After 50 failures in a row it stops and exits
`1`. A stored numeric setting that is not a number logs a warning and uses its
default.

## Start And Stop Windows Services

```powershell
sc.exe start TheWatcherServer
sc.exe stop TheWatcherServer
sc.exe start TheWatcherAgent
sc.exe stop TheWatcherAgent
```

Install and uninstall commands are in [Installation](installation.md) and
[Service Installation](service-installation.md).

## Run The Dashboard

Development dashboard:

```powershell
cd dashboard
npm.cmd install
npm.cmd run dev
```

Open:

```text
http://127.0.0.1:5173
```

Vite proxies `/api` to `http://127.0.0.1:8080`.

Initial login after the server creates its SQLite database:

```text
username: thewatcher
password: look_at_me
```

Production dashboard bundle:

```powershell
cd dashboard
npm.cmd run build
```

The production files are written to `dashboard\dist`. The C++ server does not
serve these static files yet; use a separate static host if you need a deployed
dashboard.

## First Enrollment Workflow

1. Start the server.
2. Configure the agent with `THEWATCHER_SERVER=<server-host-or-ip>`.
3. Start the agent.
4. Open the dashboard and sign in.
5. Open Pending Enrollments.
6. Approve the pending agent and assign it to a group.
7. Wait for the agent to persist the approved server public key and pinned
   fingerprint, reconnect with CURVE, submit metrics, and appear in
   Monitoring and Agents.

Rejected agents receive a rejection response on their next enrollment attempt.
Delete the rejected row to allow a fresh enrollment.

`SERVER_PUBLIC_KEY` and `SERVER_PUBLIC_KEY_FINGERPRINT` are written to
`TheWatcherAgent.conf` after approval. You only need to set them manually when
pre-pinning a known server identity before first enrollment.

## Health Checks

Check server API:

```powershell
Invoke-RestMethod http://127.0.0.1:8080/api/agents
Invoke-RestMethod http://127.0.0.1:8080/api/metrics
```

Check logs:

```powershell
Get-Content C:\ProgramData\TheWatcher\TheWatcherServer.log -Tail 50
Get-Content C:\ProgramData\TheWatcher\TheWatcherAgent.log -Tail 50
```

Run the end-to-end integration test:

```powershell
meson test -C builddir-release server_agent_integration_test --print-errorlogs
```

That test starts an isolated server, enrolls and approves an agent, verifies
CPU, memory, disk, process, network, OS, host, platform, and uptime data reach
the server, disconnects the agent, and shuts down.

## Offline Behavior

The server updates `last_seen` for every inbound agent frame. Approved connected
agents are marked disconnected when the server has not seen them for more than
`offline_after_seconds` from `server.json`.

Explicit disconnect still marks an agent disconnected immediately when the agent
ACKs the command.

## Per-Agent Thresholds

Open Agents, edit the CPU, memory, disk, or network threshold values, then use
the Thresholds button on that row. Values are percentages of that agent's
five-minute average and must be ordered warning, degraded, then critical.

The status engine still applies the absolute fallback caps: `70` is at least
warning, `85` is at least degraded, and `95` is critical.
