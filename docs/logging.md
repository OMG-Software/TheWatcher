# Logging

The server and agent use `common/SingleLog.hpp` directly. Do not add a wrapper
around `SingleLog`; it is intended to be included and used as-is.

## Log Locations

Foreground and service modes write logs beside the active config file:

```text
server config: <config-directory>\server.json
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
- file logging at `INFO` by default, set with `LOG_LEVEL` in the config or
  `--log-level <level>` on the command line (the command line wins). Levels
  are `trace`, `debug`, `info`, `notice`, `warning`, `error`, `critical` and
  `off`. `TRACE` writes an entry for every function and every heartbeat, so use
  it only while diagnosing a problem;
- live file flushing every 100 trace, debug, or info file entries;
- immediate file flushing for notice, warning, error, and critical entries.

Until the config file is read, the file level is `INFO` (or the
`--log-level` value). The config's `LOG_*` values apply as soon as it loads.

`TheWatcherServer` and `TheWatcherAgent` build a
`thewatcher::logging::LogSettings` (`common/log_settings.hpp`) from the config
and call `SetFileLogLevel(...)`, `SetLogRotation(...)` and
`SetLogFilePath(...)` on `SingleLog::GetInstance()` from their `main.cpp`
files.

## Append And Rotation

The log file is opened in append mode, so restarting a process, including after
a crash, keeps the earlier entries.

The file is rotated by size. When the next entry would take it above
`LOG_MAX_SIZE_MB`, `<log>` becomes `<log>.1`, `<log>.1` becomes `<log>.2`, and
so on up to `LOG_MAX_FILES`; the oldest file is deleted. Entries are never
split across files. Defaults are 10 MiB and 5 backups, so the log uses at most
about 60 MiB per binary. Set `LOG_MAX_SIZE_MB=0` to turn rotation off, and
`LOG_MAX_FILES=0` to keep no backups (the file is emptied at the limit).
Example:

```text
LOG_LEVEL=info
LOG_MAX_SIZE_MB=10
LOG_MAX_FILES=5
```

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
- Temperature logs sensor counts where implemented and unsupported Windows
  collection otherwise.

The agent also logs a collected metrics summary after each collection cycle,
including host, platform, uptime, CPU usage, memory usage, and disk, network,
temperature, and process counts. The agent logs queued and sent frame details
for metrics, config requests, heartbeat frames, and command acknowledgements.

The server logs every decoded inbound agent frame with identity, frame type,
payload size, and frame timestamp. Metric frames include decoded CPU, memory,
disk, network, temperature, process, host, platform, and uptime summaries before
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
