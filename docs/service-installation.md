# Service Installation

This is the focused Windows Service Control Manager command reference. Full
operator setup is in [Installation](installation.md), [Configuration](configuration.md),
and [Running](running.md).

For packaged Windows installs, use `packaging\windows\TheWatcher.iss` with
Inno Setup. The installer runs the same service install/uninstall commands
shown below and installs files under `C:\Program Files\TheWatcher`.

The server and agent run in foreground mode by default. Use the service flags
only when installing or running under Windows Service Control Manager.

## Server Service

Install:

```powershell
.\builddir-release\server\TheWatcherServer.exe --install-service --config C:\ProgramData\TheWatcher\server.json
```

Start and stop:

```powershell
sc.exe start TheWatcherServer
sc.exe stop TheWatcherServer
```

Uninstall:

```powershell
.\builddir-release\server\TheWatcherServer.exe --uninstall-service
```

The installed command line runs the same executable with `--service`.

## Agent Service

Create or edit `C:\ProgramData\TheWatcher\TheWatcherAgent.conf`:

```text
THEWATCHER_SERVER=<server-host-or-ip>
```

The agent learns `SERVER_PUBLIC_KEY` and `SERVER_PUBLIC_KEY_FINGERPRINT` from
the approved enrollment response and persists them. Add those values manually
only when pre-pinning a known server identity before first enrollment.

Install:

```powershell
.\builddir-release\agent\TheWatcherAgent.exe --install-service --config C:\ProgramData\TheWatcher\TheWatcherAgent.conf
```

You can also pass overrides at install time. They are added to the service
command line, so they apply each time the service starts, but they are not
written to the config file:

```powershell
.\builddir-release\agent\TheWatcherAgent.exe --install-service --config C:\ProgramData\TheWatcher\TheWatcherAgent.conf --server tcp://monitor.local:5555 --enrollment tcp://monitor.local:5556
```

Start and stop:

```powershell
sc.exe start TheWatcherAgent
sc.exe stop TheWatcherAgent
```

Uninstall:

```powershell
.\builddir-release\agent\TheWatcherAgent.exe --uninstall-service
```

## Multiple Instances

Use `--service-name <name>` consistently with install, service mode, and
uninstall:

```powershell
.\builddir-release\agent\TheWatcherAgent.exe --install-service --service-name TheWatcherAgentLab --config C:\ProgramData\TheWatcher\lab-agent.conf
sc.exe start TheWatcherAgentLab
.\builddir-release\agent\TheWatcherAgent.exe --uninstall-service --service-name TheWatcherAgentLab
```

## Logs

Runtime logs are written beside the selected config file:

```text
C:\ProgramData\TheWatcher\TheWatcherServer.log
C:\ProgramData\TheWatcher\TheWatcherAgent.log
```

## Non-Windows

The service flags return an explanatory error outside Windows. Linux and BSD
service management should be handled by the host init system, such as systemd,
rc.d, or another supervisor, using normal foreground mode and explicit
`--config` paths.
