# Logging

The server and agent use `common/SingleLog.hpp` directly. Do not add a wrapper
around `SingleLog`; it is intended to be included and used as-is.

## Log Locations

Foreground and service modes write logs beside the active config file:

```text
server config: <config-directory>\TheWatcherServer.conf
server log:    <config-directory>\TheWatcherServer.log

agent config:  <config-directory>\TheWatcherAgent.conf
agent log:     <config-directory>\TheWatcherAgent.log
```

When a config path has no parent directory, logs are written to the current
working directory.

Recommended Windows service locations:

```text
C:\ProgramData\TheWatcher\TheWatcherServer.log
C:\ProgramData\TheWatcher\TheWatcherAgent.log
```

## Levels

Each binary configures its own logging before runtime work begins:

- console logging at `INFO`;
- file logging at `TRACE`;
- live file flushing every 100 trace, debug, or info file entries;
- immediate file flushing for notice, warning, error, and critical entries.

`TheWatcherServer` and `TheWatcherAgent` call
`SingleLog::GetInstance().SetConsoleLogLevel(...)`,
`SetFileLogLevel(...)`, and `SetLogFilePath(...)` directly from their
`main.cpp` files.

## Macros

Runtime log writes should use the macros provided by `SingleLog.hpp`:

```cpp
LOG_INFO("Agent started");
LOGF_DEBUG("Queued command id=%s type=%u", command_id.c_str(), type);
LOG_WARNING("Enrollment stopped before approval");
```

Use `LOG_` macros for literal or already-built strings. Use `LOGF_` macros
when formatting values.

`LOG_FUNCTION_TRACE` creates a scope logger that emits entry and exit trace
messages. Use it on lifecycle, setup, config, and infrequent control-flow
functions. Avoid it in hot loops and per-frame handlers. Collector update
methods use one scope trace per collection cycle and concise trace/debug
summary lines so operators can see whether collection is running without logging
every mount, process, sensor, or network interface as a separate normal-path
entry.

## Collector Diagnostics

Collectors write file-level `TRACE` and `DEBUG` diagnostics through the same
agent logger:

- CPU logs unsupported platforms, missing valid samples, and usage summaries.
- Memory logs unavailable OS APIs or `/proc/meminfo` and usage summaries.
- Disk logs mount counts after each update.
- Network logs interface counts on supported platforms and unsupported-platform
  notices otherwise.
- Process logs discovered and reported process counts.

The agent also logs a collected metrics summary after each collection cycle,
including host, platform, uptime, CPU usage, memory usage, and disk, network,
and process counts. The agent logs queued and sent frame details
for metrics, config requests, heartbeat frames, and command acknowledgements.

The server logs every decoded inbound agent frame with identity, frame type,
payload size, and frame timestamp. Metric frames include decoded CPU, memory,
disk, network, process, host, platform, and uptime summaries before
the metrics are stored. Heartbeat frames include the agent id and heartbeat
timestamp.

## Startup Diagnostics

Server foreground mode waits for the server worker thread to report startup
success before logging:

```text
Running. Press Ctrl+C to stop.
```

If ZeroMQ binding, CURVE setup, API startup, or another startup step fails, the
server logs a critical `Startup failed: ...` message and exits non-zero.

## Useful Commands

```powershell
Get-Content C:\ProgramData\TheWatcher\TheWatcherServer.log -Tail 100
Get-Content C:\ProgramData\TheWatcher\TheWatcherAgent.log -Tail 100
```

Search for enrollment:

```powershell
Select-String -Path C:\ProgramData\TheWatcher\TheWatcherServer.log -Pattern "New agent|pending|approved|rejected"
```

Search for agent runtime errors:

```powershell
Select-String -Path C:\ProgramData\TheWatcher\TheWatcherAgent.log -Pattern "Fatal|error|warning"
```

## Service Mode

Windows service mode uses the same log path rules as foreground mode. Always
install services with `--config <path>` so logs land in a stable writable
directory.

## CLI Output

The `--help`, `--genkey`, `--install-service`, and `--uninstall-service`
commands keep concise console output for scripting and operator feedback.
Runtime diagnostics go through `SingleLog`.

## Untrusted Text In Logs

The server logs strings that agents and enrollment requests supply (agent
ids, hostnames, mount points, interface and process names, log-match paths,
indicator names and command ACK text). Each one passes through
`thewatcher::text::sanitize_log_text` (`common/text_safety.hpp`) first:
control characters (including CR, LF and ESC), the Unicode line separators
U+0085, U+2028 and U+2029, and invalid UTF-8 bytes become `?`, and the text is
cut to 64 bytes without splitting a character. A host cannot start a fake log
line or inject terminal escapes. New code that logs peer-supplied text must
use the same function.
