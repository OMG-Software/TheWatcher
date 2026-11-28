# Development

This document covers the repository layout, common build/test commands, and the
work required to extend TheWatcher end to end.

## Repository Layout

```text
agent/              Agent runtime, config, enrollment, collectors.
agent/collectors/   CPU, memory, disk, temperature, process, network collectors.
agent_tests/        Agent config and collector contract tests.
common/             Shared protocol, commands, metrics, crypto, logging, service helpers.
common_tests/       Tests for shared helpers.
dashboard/          React + TypeScript + Vite dashboard.
dashboard_tests/    Dashboard test helpers or future browser tests.
docs/               Operator and developer documentation.
integration_tests/  End-to-end server-agent lifecycle tests.
plans/              dev-task-planner task plans.
scripts/            Cross-platform build-environment setup scripts.
server/             Server runtime, API, storage, ZAP auth, config.
server_tests/       Store, lifecycle, and ZAP tests.
test_support/       Header-only helpers shared by the test suites (fixtures,
                    free ports, timing, background calls).
subprojects/        Meson wrap files and packagefile overlays for libsodium,
                    libzmq, libcbor, and WrapDB-tracked dependencies.
```

## Core Build Targets

```bash
meson compile -C builddir-release TheWatcherServer TheWatcherAgent
meson test    -C builddir-release config_test
meson test    -C builddir-release store_test
meson test    -C builddir-release status_engine_test
meson test    -C builddir-release server_lifecycle_test
meson test    -C builddir-release zap_handler_test
meson test    -C builddir-release server_agent_integration_test --print-errorlogs
meson test    -C builddir-release --print-errorlogs
```

Dashboard:

```powershell
cd dashboard
npm.cmd install
npm.cmd run build
npm.cmd test
```

## Coding Standards

- C++ code is C++20.
- Format C++ with the repository `.clang-format`. CI runs
  `clang-format --dry-run --Werror` (version 22.1.5, installable with
  `pip install --only-binary :all: clang-format==22.1.5`) over every tracked
  first-party C++ file (`*.cpp`, `*.hpp`, `*.h` outside `subprojects/`) and
  fails the build on any difference. Third-party overlays under
  `subprojects/packagefiles/` are not checked. Whole-tree reformatting commits are listed in
  `.git-blame-ignore-revs`; run
  `git config blame.ignoreRevsFile .git-blame-ignore-revs` once so
  `git blame` skips them.
- SonarCloud automatic analysis reads `.sonarcloud.properties`. Test
  directories and `*.test.ts(x)` files are excluded from duplication
  detection only (BDD scenarios repeat setup deliberately); keep shared
  setup in helpers when it carries logic.
- Keep top-level C++ namespace as `thewatcher`.
- Prefer structured types in `common/metrics.hpp`, `common/protocol.hpp`, and
  `common/commands.hpp` over ad hoc strings.
- Use BDD-style tests with `GIVEN`, `WHEN`, and `THEN`.
- To check that a call such as `stop()` or `run()` returns within a deadline,
  run it with `thewatcher::test::BackgroundCall` from
  `test_support/background_call.hpp`, not `std::async`. It runs the call on a
  `std::jthread` and rethrows the call's exception from `get()`. Destruction
  joins the thread, so a test that stops waiting must first make the call
  return.
- Add tests before behavior changes.
- Update docs for every behavior or workflow change.
- For security-sensitive changes, update [Threat Model](threat-model.md) with
  the affected asset, trust boundary, mitigation, and residual risk.
- Bump the `version` in `meson.build` and update `CHANGELOG.md` for each
  change set.
- Meson builds ZeroMQ and libsodium as local static subprojects from
  `subprojects/*.wrap` plus `subprojects/packagefiles/`; do not add vcpkg,
  pkg-config, or system package assumptions for these dependencies.
- Keep persistence/status tests on the lighter `server_store_lib` target when
  they do not need ZeroMQ. Use `server_lib` only for tests that exercise the
  full server runtime.
- Keep `httplib` route callbacks self-contained. Shared authorization,
  session, and command helpers should live on `ApiServer` and callbacks should
  capture `this` or values, not references to stack-local helper lambdas from
  route setup.

## Adding A New Collector End To End

Use this checklist when adding a metric family such as GPU, filesystem inode
usage, services, packages, or custom application health.

1. Define the metric shape in `common/metrics.hpp`.

   Add a struct with stable field names and a
   `NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE` mapping. Add the struct to
   `SystemMetrics`.

2. Add wire/API compatibility tests.

   Update or add tests that serialize and inspect `SystemMetrics`. If the new
   metric affects integration behavior, extend
   `integration_tests/server_agent_integration_test.cpp`.

3. Implement the collector.

   Add `agent/collectors/<name>_collector.hpp` and `.cpp`. Inherit from
   `thewatcher::agent::Collector` and implement:

   ```cpp
   std::string_view name() const noexcept;
   void update(SystemMetrics& metrics);
   ```

   Keep platform-specific code behind preprocessor guards. The agent should
   still compile on Windows, Linux, and BSD.

4. Register the collector with Meson.

   Update `agent/collectors/meson.build` with the new source and header so
   the static `agent_collectors` library picks them up.

5. Add the collector to the agent runtime.

   Register it in `Agent::Agent()` in `agent/agent.cpp`. If it needs runtime
   settings, add those settings to `common/collector_config.hpp`,
   `common/commands.hpp`, `server/store.hpp`, `server/store_sqlite.cpp`,
   `server/api.cpp`, `dashboard/src/models.ts`, and the Agents Configure
   modal. `ConfigUpdate` is the agent-facing carrier for these settings.

6. Update dashboard models and status mapping.

   Update `dashboard/src/models.ts` for the API shape, `dashboard/src/status.ts`
   for health classification, and the matching file under `dashboard/src/pages/`
   or `dashboard/src/components/` if the UI needs new controls or columns. Collector thresholds should be absolute values in the
   same units the collector reports: percentages for CPU, memory, and disks;
   combined megabits per second for network interfaces; exact executable counts
   for process watches.
   If the new collector becomes an alerting indicator, also update
   `server/status_engine.cpp` and `server_tests/status_engine_test.cpp`.

7. Add tests.

   Add focused collector tests in `agent_tests/` or extend
   `collector_contract_test.cpp`. Add dashboard tests when health classification
   changes. Extend integration tests when the new metric must be proven through
   the server API.

8. Update docs.

   Update [Collector Contract](collector-contract.md), [Architecture](architecture.md),
   [Dashboard](dashboard.md), and `CHANGELOG.md`.

9. Run verification.

   ```powershell
   clang-format -i <changed-cpp-files>
   meson test -C builddir-release collector_contract_test server_agent_integration_test --print-errorlogs
   cd dashboard
   npm.cmd run build
   npm.cmd test
   ```

## Adding A New Server Command

1. Add the command enum and payload struct in `common/commands.hpp`.
2. Add an API route in `server/api.cpp` that validates input and queues a
   `CommandMessage`.
3. Handle the command in `Agent::handle_command()` in `agent/agent.cpp`.
4. If the command changes persistent state, update `server/store.hpp`,
   `server/store_sqlite.cpp`, the dashboard model, and API tests.
5. Add BDD tests and update docs.

## Adding A New REST API Field

1. Add the persistent or computed field in the server model.
2. Serialize it in `agent_to_json()` or metric JSON.
3. Update `dashboard/src/models.ts`.
4. Update `dashboard/src/status.ts` or the matching file under
   `dashboard/src/pages/` or `dashboard/src/components/` if displayed.
5. Run dashboard build/tests and relevant server tests.

## Third-Party Dependencies

Prefer WrapDB-tracked Meson wraps when they preserve behavior. Keep custom
packagefile overlays only where the project needs custom build settings
(currently libsodium and libzmq, both built with CURVE-enabled static linkage)
or the dependency is not available in WrapDB.

Current dependencies pulled in via `subprojects/*.wrap`:

- `libzmq` (with `subprojects/packagefiles/libzmq` overlay)
- `libsodium` (with `subprojects/packagefiles/libsodium` overlay)
- `libcbor` (with `subprojects/packagefiles/libcbor` overlay providing
  pre-generated headers)
- `cppzmq`, `sqlite3`, `nlohmann_json`, `cpp-httplib`, `catch2-with-main`
  (WrapDB).

## Release Checklist

1. Add or update BDD tests.
2. Implement the change.
3. Run focused tests.
4. Run relevant integration tests.
5. Format C++ code.
6. Update docs.
7. Bump the `version` in `meson.build`.
8. Add a `CHANGELOG.md` entry.
9. Run final build/test commands for changed components.
