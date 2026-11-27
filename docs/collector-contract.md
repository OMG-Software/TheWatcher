# Agent Collector Contract

Collectors are the agent-side modules that populate one shared
`SystemMetrics` snapshot before the agent sends it to the server.

## Interface

All collectors inherit from `thewatcher::agent::Collector` in
`agent/collectors/collector.hpp`:

```cpp
std::string_view name() const noexcept;
void update(SystemMetrics& metrics);
```

`name()` returns a stable diagnostic identifier. `update(SystemMetrics&)`
mutates the provided snapshot in place.

## Current Collectors

| Collector | Files | Metrics fields |
| --- | --- | --- |
| CPU | `cpu_collector.hpp/.cpp` | `metrics.cpu` |
| Memory | `memory_collector.hpp/.cpp` | `metrics.memory` |
| Disk | `disk_collector.hpp/.cpp` | `metrics.disks` |
| Process | `process_collector.hpp/.cpp` | `metrics.top_processes` |
| Network | `network_collector.hpp/.cpp` | `metrics.networks` |

Temperature collection is intentionally not built. The `metrics.temperatures`
field remains in `SystemMetrics` so older agents and stored rows can still be
decoded, but current agents do not populate it and the status engine does not
raise temperature alerts.

On Windows, the network collector reports IP Helper adapters that line up with
`ipconfig` output. Lower-layer devices such as packet schedulers, WFP native MAC
layer filters, Hyper-V virtual switch plumbing, 6to4/tunnel adapters, and bridge
devices are filtered before metrics are emitted.

### CPU and memory collector behaviour

- `CpuCollector` takes a baseline tick sample in its constructor. The first
  `update()` waits until that baseline is at least 100 ms old, so a new or
  restarted collector reports a real usage value instead of 0%. An `update()`
  less than 100 ms after the previous sample reports the previous result.
  Tick sources: `/proc/stat` (Linux), `GetSystemTimes` (Windows),
  `kern.cp_time`/`kern.cp_times` (FreeBSD), `KERN_CPTIME`/`KERN_CPTIME2`
  (OpenBSD), `kern.cp_time` (NetBSD). Load averages come from `/proc/loadavg`
  or `getloadavg`. `cpu_percent_from_delta()` returns 0 for counters that
  did not advance or went backwards.
- `MemoryCollector` never lets a subtraction wrap (`saturating_sub`,
  `swap_usage_from_used`, `swap_usage_from_free`). Swap always satisfies
  `used + free == total`, and a host with no swap reports 0/0/0. Swap
  sources: `/proc/meminfo` (Linux), the page files via `EnumPageFilesW`
  (Windows), `vm.swap_info` (FreeBSD), `struct uvmexp` (OpenBSD) and
  `struct uvmexp_sysctl` (NetBSD). DragonFly reports no swap.
  OpenBSD and NetBSD code is written against the man pages and not yet
  compiled in CI.

The agent also fills static system fields in `Agent::fill_static_info()`:

```text
os_name
os_version
hostname
platform
uptime_seconds
```

## Collector Rules

- A collector owns any previous-sample state needed for rate calculations.
- A collector should replace its full vector field each update unless it is
  intentionally merging values.
- A collector must not send frames or talk to the server directly.
- A collector should avoid throwing for ordinary missing platform data. Prefer
  empty vectors or zero values when a metric is unavailable.
- Platform-specific implementations must keep Windows, Linux, and BSD builds
  compiling.
- Keep field names stable. Dashboard and API users consume these JSON names.

## Runtime Settings

The current runtime settings are:

- `collection_interval`: seconds between periodic metrics submissions.
- `heartbeat_interval`: seconds between heartbeat frames. Valid range is 1 to
  60 seconds. The default is 5 seconds.
- `process_limit`: number of top processes captured by `ProcessCollector`.
- `collector_config`: per-agent collector thresholds, enabled fixed disks,
  enabled network interfaces, process watches, and consecutive-reading counts.

The server persists these settings and returns them to the agent in
`CONFIG_UPDATE` after the agent sends `CONFIG_REQUEST`. Collection interval is
only for collector snapshots; heartbeat interval is configured separately.

`collector_config` is defined in `common/collector_config.hpp`. CPU, memory,
and disk thresholds are absolute percentages. Network thresholds are combined
receive plus transmit megabits per second per interface. Process watches use
exact executable names and expected instance counts; `ProcessCollector` always
includes matching watched processes in `metrics.top_processes`, even when they
are outside the normal top-N sample.

## Adding A Collector

1. Add the metric struct to `common/metrics.hpp` with `to_cbor` / `from_cbor`
   specialisations (the CBOR wire format), and add the field to
   `SystemMetrics`.
2. Add the JSON mapping for the struct and the new `SystemMetrics` field to
   `server/api_json.hpp` (the REST API form).
3. Add collector source/header files under `agent/collectors/`.
4. Add the files to `agent/collectors/meson.build`.
5. Register the collector in `Agent::Agent()` in `agent/agent.cpp`.
6. Add or update `agent_tests/collector_contract_test.cpp`.
7. Update `integration_tests/server_agent_integration_test.cpp` if the metric
   must be proven through the server API.
8. Update `dashboard/src/models.ts`, `dashboard/src/status.ts`, and
   `dashboard/src/main.tsx` if the metric is displayed.
9. Update docs and changelog.

More end-to-end extension guidance is in [Development](development.md).

## Verification

Run collector and integration tests:

```powershell
meson test -C builddir-release collector_contract_test server_agent_integration_test --print-errorlogs
```

If dashboard status mapping changes:

```powershell
cd dashboard
npm.cmd run build
npm.cmd test
```
