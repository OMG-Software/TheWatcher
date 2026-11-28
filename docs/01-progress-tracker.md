# Progress Tracker

This document is the release progress source of truth for TheWatcher. Task-level
implementation plans remain in `plans/`; they are retained as work history, not
as milestone trackers.

## Consolidation Notes

- No previous release progress tracker existed in `docs/`.
- Current Done items are consolidated from `README.md`, `docs/architecture.md`,
  `docs/dashboard.md`, `docs/collector-contract.md`, `docs/development.md`, and
  `CHANGELOG.md`.
- Keep future release planning updates in this file instead of adding separate
  roadmap, TODO, or milestone tracker documents.

## Alpha

Status: in progress.

### Done

- C++20 Meson project foundation is in place with static first-party targets.
- ZeroMQ, libsodium, libcbor, SQLite, nlohmann-json, cpp-httplib, cppzmq, and
  Catch2 are managed through Meson subprojects or package overlays.
- Agent enrollment exists with generated agent identity, CURVE keys, server
  public-key pinning, and approved/rejected enrollment states.
- Agent-to-server data flow exists for heartbeats, metrics, config requests,
  config updates, operator commands, and command acknowledgements.
- User-space agent collectors report CPU, memory, disk, process, and network
  metrics while preserving cross-platform build expectations.
- Server persistence covers agents, metric snapshots, users, groups, sessions,
  status history, alerts, settings, views, runbooks, and collector
  configuration.
- REST API coverage exists for sessions, agents, pending enrollments, metrics,
  groups, users, alerts, views, runbooks, maintenance, and operator commands.
- Dashboard operator workflows exist for monitoring, agent enrollment,
  per-agent configuration, alerts, user/group access control, views, runbooks,
  maintenance, and agent commands.
- Windows service installation commands and foreground running workflows are
  documented.
- Windows, Linux, and BSD build-environment setup documentation exists.
- Threat model, architecture, dashboard, collector contract, installation,
  configuration, running, logging, service installation, and development docs
  exist.

### TODO

- Add a repeatable alpha smoke-test checklist that covers server startup, agent
  enrollment, metrics submission, dashboard login, alert creation, and command
  dispatch.
- Define alpha exit criteria for supported operating systems, required test
  suites, and manual verification.
- Decide the minimum supported dashboard deployment path for alpha demos.
- Review current docs for operator-facing gaps before declaring alpha complete.
- Triage known issues into alpha blockers, beta backlog, or post-v1 backlog.

## Beta

Status: planned.

### Done

- Cross-platform build scripts and documentation already cover Windows, Linux,
  and BSD setup paths.
- CI artifact changelog entries indicate archived build output links for Linux,
  Windows, and FreeBSD builds.
- Production dashboard bundles can be built with Vite, although the C++ server
  does not serve them yet.
- High-availability topology documentation exists as a planning foundation.

### TODO

- Validate the PostgreSQL store (`PostgresStore`) against the SQLite store
  behaviour. It is implemented and selectable with `DB_TYPE=postgres`, but it
  is only built when libpq is available and has less test coverage.
- Provide a supported production dashboard hosting model, either through the
  server or documented reverse-proxy/static-hosting guidance.
- Add upgrade, downgrade, backup, and restore procedures for server data.
- Define retention and cleanup policy for metrics, status history, alerts, and
  sessions.
- Add operator audit logging for sensitive actions such as login, enrollment,
  user management, group changes, maintenance, and delete operations.
- Run load and soak testing for agent count, metric volume, alert volume, and
  dashboard polling.
- Package or script repeatable install/uninstall flows for all target
  platforms.
- Finalize notification/webhook production behavior and failure handling.

## V1 Production Release

Status: planned.

### Done

- Security baseline work exists for browser security headers, authenticated
  origin checks, webhook URL validation, config input bounds, CURVE key use, and
  server-key fingerprint pinning.
- Operator documentation covers installation, configuration, running, logging,
  service installation, architecture, dashboard behavior, development workflow,
  collector contracts, and threat modeling.
- Release checklist guidance exists in `docs/development.md`.

### TODO

- Freeze the v1 API, protocol, database schema migration, and dashboard
  compatibility contracts.
- Publish signed release artifacts with checksums and reproducible build notes.
- Document production hardening for TLS termination, secret storage, firewall
  rules, service accounts, file permissions, backups, and key rotation.
- Complete disaster recovery runbooks, including restore testing and operator
  acceptance criteria.
- Define support policy, version lifecycle, upgrade cadence, and rollback
  expectations.
- Confirm performance targets for supported agent counts, polling intervals,
  alert latency, dashboard response time, and storage growth.
- Complete final accessibility, usability, and browser compatibility passes for
  the dashboard.
- Run final release-candidate verification across Windows, Linux, and BSD.
