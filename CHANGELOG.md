# Changelog

All notable changes to TheWatcher are recorded here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

## [Unreleased]

## [0.9.0] - 2026-11-27

### Security

- Login throttling no longer lets a client-supplied or malformed
  `X-Forwarded-For` / `X-Real-IP` choose or poison the throttle bucket.
  `TRUSTED_PROXIES` now accepts CIDR ranges, `X-Real-IP` is preferred, all
  `X-Forwarded-For` lines are read (any empty or invalid entry makes the header
  malformed), and a malformed forwarded address gets
  its own bucket instead of the proxy's address. This only holds if the
  trusted proxy overwrites or strips any client-supplied `X-Real-IP` and
  `X-Forwarded-For` (#153).
- Unknown and disabled usernames are verified against a dummy Argon2 hash to
  reduce timing-based username enumeration (not eliminate it), and the login rate
  limit check and failure count are one atomic step, so concurrent guesses
  cannot exceed the limits (#154).
- Sessions are revoked when a password changes (the caller's own session is
  kept on a self-service change), and when a user is disabled or deleted;
  expired sessions are purged periodically. New store methods
  `delete_sessions_for_user` and `delete_expired_sessions` (SQLite and
  PostgreSQL) (#145).
- Authentication and authorization events are audit logged as `AUDIT` lines
  at NOTICE: login success/failure/lockout, logout, every 403, user
  create/disable/enable/delete, password change and session revocation (#70).
- Hardened the new login and session code after review: a successful login
  releases only its own throttle slot, an empty forwarded header and the
  wrong address family no longer fall back to the proxy address or trust
  the wrong peers, a login that verified an old password cannot create a
  session after a reset or disable, the password change and session
  revocation are atomic, and the dummy hash is generated randomly at startup.
  New store methods `create_session_if_credentials_current` and
  `update_user_password_and_revoke_sessions` (#145, #153, #154).
- Login throttle follow-ups: an attempt that outlives its throttle window no
  longer decrements a newer window's slot (reservations remember the window
  they were counted in), and an empty first `X-Forwarded-For` header line is
  malformed instead of being skipped when the lines are joined (#153, #154).
- Review follow-ups on user and session handling: disabling a user and
  revoking their sessions is now one atomic store operation
  (`disable_user_and_revoke_sessions`), enabling an unknown user id returns
  404, the logout audit line is written only after the session is deleted, an
  invalid entry anywhere in `X-Forwarded-For` makes the header malformed, and
  the expired-session purge rechecks that it is still due and advances its
  clock only on success (#145, #153, #70).
- Enabling a user now relies on a single UPDATE that reports the rows it
  changed (`enable_user` returns a count), so enabling an id deleted a moment
  earlier returns 404 instead of success (#145).
- On PostgreSQL a login now locks the user row (`FOR SHARE`) before inserting
  its session, and the password reset and disable run UPDATE then DELETE as
  two statements in one transaction, so a login racing a reset or disable
  can no longer leave a session behind. A password change for a user deleted
  mid-request now returns 404 instead of being audited as a success
  (`update_user_password_and_revoke_sessions` returns no value for an unknown
  user) (#145).
- The agent no longer trusts the server key in an enrollment reply: it must
  be 40 Z85 characters decoding to a 32-byte key, and the config writer
  refuses any value containing CR or LF. Before, a crafted key could inject
  `SERVER_ADDRESS` or `ALLOW_INSECURE` lines into the saved config (#148).
- The agent config is now written atomically through an owner-only temporary
  file and renamed over the old one; write errors are reported. On Windows the
  file gets a protected DACL (SYSTEM, Administrators, owner) instead of the
  parent directory's inherited rights (#149).
- Fixed a server crash (NULL dereference) and heap over-read when a metrics
  or collector-config payload has a CBOR `null`/`true`/`false`/`undefined`
  or a half/single-precision float where a double is expected. All float
  widths now decode; simple values are rejected (#139).
- Fixed an agent crash when a monitored log has a long line: `std::regex`
  recursion overflowed the thread stack (about 40 KB was enough). Only the
  first 4 KiB of a line is matched, and matching runs on a helper thread
  with a fixed 64 MB stack (#140). A match that lies only after the first
  4 KiB of a line is no longer reported, and `$` does not match at the cut.
  A pattern that nests groups more than 16 levels deep is skipped with a
  warning; 100 nested groups overflowed the 64 MB stack.
- **Behaviour change:** the data plane no longer trusts the ROUTER routing
  id. Agents no longer set it (libzmq generates one), and the server sends
  commands only to the connection that last sent an authenticated frame for
  that agent. Before, another approved agent could connect with a victim's
  agent_id as its routing id and receive the victim's commands or block its
  connection. A command for an agent that has not sent a frame since the
  server started is now dropped and logged (#141).
  **Upgrade order:** upgrade the server before the agents. A server older
  than this release sends commands to the routing id `agent_id`, which this
  agent no longer sets, so its commands do not reach upgraded agents. A
  server from this release still works with 0.8.0 agents.
  The server also forgets an agent's route when the agent is marked
  offline.
- The webhook URL check rejects non-canonical IPv4 forms that the resolver
  still reads as addresses (octal `0251.0376.0251.0376`, hex `0x7f000001`,
  decimal `2130706433`, short `127.1`, `0`, trailing dot) and IPv6 forms
  that embed or translate to IPv4 (`::a.b.c.d`, `::ffff:0:a.b.c.d`, NAT64
  `64:ff9b::/96` and `64:ff9b:1::/48`, 6to4 `2002::/16`), plus `fec0::/10`
  and `100::/64` (#142).
- The browser Origin check accepts the loopback exception only for real
  `127.0.0.0/8` addresses; DNS names such as `127.attacker.example` no
  longer match (#164).
- Changing your own password now requires `current_password` in the
  `PUT /api/users/:id/password` body, for every role including a global
  admin. A missing or non-string value is 400; a wrong value is 403 and
  changes nothing. A global admin resetting another user's password needs no
  current password. Wrong guesses count against the login throttle under the
  login keys, so a stolen session cannot brute-force the password, and are
  audited as `password_change` `failure` (or `locked_out`, 429). The dashboard
  asks for the current password only on your own account, and now shows the
  server's reason when a change is refused (#145).
- Review follow-ups on the password change and sessions (#145): a username
  containing a NUL byte is rejected at login and user creation (it bypassed the
  login throttle keys); a self-service password change is applied only if the
  stored hash is still the one that was verified, in one transaction with the
  revocation, and an admin reset that landed first makes it fail with 409 (new
  Store method `update_user_password_if_current_and_revoke_sessions`);
  enabling a user revokes leftover sessions only on a real disabled-to-enabled
  change and a repeated enable is harmless (new Store method
  `enable_user_and_revoke_sessions`); a wrong current password is audited only
  as `password_change` `failure`; expired sessions are also purged by the
  server's periodic job; the SQLite session transactions use the RAII guard so a
  failing ROLLBACK cannot mask the original error; on PostgreSQL
  `set_agent_groups` and `set_user_groups` lock the parent row first, so two
  concurrent replacements cannot leave the union of both sets. The dashboard
  keeps the Change Password form open and shows the server's reason when a
  change is refused, hides the Password button on other users' rows unless you
  are a global admin, and stops waiting for an error body after two seconds.
  The form's fields are locked while it saves, and an answer that arrives after
  the form was closed (or replaced by a new one) is discarded instead of
  reopening or overwriting it. The rolled-back-reset test now holds the reset
  inside its store call with a SQLite write lock, and fails unless a login
  attempts its call during the reset and its session survives the rollback.
  The PostgreSQL lock tests check that the fixture connection blocks the exact
  backend under test (new `PostgresStore::backend_pid`), so an unrelated lock
  wait no longer counts. The `auth_sessions` suite gets a 120-second test
  timeout like `login_rate_limit`, because its real Argon2 work exceeded
  meson's 30-second default on the Linux and FreeBSD CI runners.
- Session tokens are stored as their SHA-256 (lowercase hex) instead of raw, so a
  database copy or backup no longer contains usable logins. The stores hash
  inside `create_session`, `get_session`, `delete_session`,
  `delete_sessions_for_user` and `create_session_if_credentials_current`;
  the `Store` interface is unchanged. **Upgrading signs everyone out once:**
  on first start the server deletes every existing session and records that in
  `server_settings` (key `session_tokens_hashed`), in the same transaction,
  so it never runs again (SQLite and PostgreSQL). During a rolling upgrade of a
  shared PostgreSQL database, a server still on the old version writes raw
  tokens that the new version ignores until they expire (#145).

### Fixed

- The status engine is now one long-lived object owned by the server instead
  of being rebuilt for every metrics frame, so stale-metric detection and the
  anomaly baseline cache work and metrics are no longer pruned on every frame
  (#43).
- One maintenance state machine now serves manual, timed and scheduled
  maintenance. Entering maintenance marks the agent's real indicators Blue
  (not the phantom `disk`, `network`, `processes`); Blue no longer suppresses
  the first alert after maintenance ends; scheduled windows go through the
  same enter/exit code as manual maintenance (#44).
- Timed manual maintenance (`until_ms`) now ends when its time passes (#45).
- The dashboard `offline_after_seconds` setting now takes effect (the setting
  page shows the real default, 60), and the offline timeout per agent is
  `max(setting, 3 x heartbeat_interval)`, so a slow heartbeat no longer makes
  an agent flap offline (#46).
- Review follow-ups for the status engine: command routes expire with the same
  per-agent offline timeout as the offline scan; an acknowledged PAUSE keeps
  the maintenance reason and expiry set by the API instead of making timed
  maintenance open ended; maintenance resets stale-value clocks; a saved
  `offline_after_seconds` overrides the config value in both directions (0
  disables, a positive value enables even if the config disables it); the engine drops
  state of deleted agents; expiring timed maintenance inside an active window
  hands over to the window without a gap.
- A NaN or infinite reading no longer raises a "grey" alert: Grey means no
  data and never alerts (#55).
- Alerts, status history, log matches, expired silences and ended
  maintenance windows are now pruned by an hourly server job that does not
  depend on metrics arriving. New settings `status_history_retention_days`,
  `alerts_retention_days` and `log_matches_retention_days` (#76).
- All alerts now go through one pipeline that applies maintenance, silences,
  severity validation, de-duplication, the runbook link and the webhook.
  Log-match alerts no longer fire during maintenance, offline and log-match
  alerts now send the webhook and offline alerts honour silences, and 100
  matching log lines leave one open alert (#52).
- Alert webhooks and scheduled reports are delivered by a worker thread with a
  bounded queue and timeouts, so a slow webhook no longer stalls the data
  plane (#48).
- The scheduled report reads its last-sent time at startup, so a restart in
  the scheduled hour does not send it twice. A non-2xx response is a failure
  and is retried with backoff; invalid `reports.*` values no longer throw
  (#49). The `reports.*` settings are documented.
- Commands for an agent that is not connected now get `409` from the API
  instead of being dropped after a success reply, and the ROUTER reports
  undeliverable commands (#54). The check uses the server's live routes, so
  an agent stored as connected before a restart is also refused until it
  sends a frame, and every command route (`command`, `set_interval`,
  `set_process_limit` too) checks it before changing or queuing anything.
  Commands are sent without blocking on a full send queue.
- `GET /api/settings` no longer fails on a malformed `reports.hour` or
  `reports.day_of_week`, and `reports.last_sent_ms` with trailing junk reads
  as never sent (#49).
- An operator DISCONNECT no longer raises a red "Agent has gone offline"
  alert that escalates; the agent is marked operator-disconnected until it
  sends a frame again (#156).
- Added behavioural tests for `ReportGenerator` (part of #137).
- Log tailing (#90): each indicator keeps its own read offset, so two
  indicators on one file both match; a monitor starts at the end of an
  existing file instead of replaying its history as new alerts (a file that
  appears later is read from its first byte); a line the writer has not
  finished is matched whole once it ends; the carriage return of CRLF lines is
  dropped so `$`-anchored patterns match; a replaced, deleted or truncated
  file is read again from its start. A regex engine failure on one line (for
  example MSVC `error_stack`) skips that line and is logged once per poll,
  instead of stalling the file.
- Log patterns that can backtrack exponentially are rejected (#169). The
  server answers 400 when a collector config is saved with one and the agent
  skips it with a warning. Refused: back-references; a repeated group that
  contains a quantifier or alternation, such as `(a+)+` or `(a|aa)*`;
  overlapping unbounded quantifiers back to back, such as `.*.*`; and more
  than three unbounded quantifiers (`*`, `+`, `{n,}`) in one pattern.
- Dashboard: `tsc` no longer writes `.js`/`.d.ts` files into `dashboard/src`
  (`noEmit`), the 21 committed compiled twins are removed, and vitest runs
  only `*.test.ts(x)`. Before, Vite served the stale twins and every test ran
  twice, once from its compiled copy (#103).
- Agent enrollment recovers after a reply timeout: each attempt uses a new
  REQ socket, so a late server is reached without restarting the agent (#86).
- An enrollment reply that cannot be decoded is logged and retried instead
  of terminating the agent (#160). An approved reply with an invalid server
  key or a fingerprint that does not match still stops enrollment with an
  error.
- IPv6 server addresses such as `[2001:db8::1]:5555` are parsed correctly in
  the agent config; an unbracketed IPv6 address is rejected with a clear
  error (#98).
- Alert notes longer than 4,096 bytes are cut on a UTF-8 character
  boundary. Oversize `runbook_markdown` (collector config) and
  `reports_webhook_url` (settings) are rejected with 400 instead of being
  cut. JSON responses replace invalid UTF-8 already stored with U+FFFD, so
  one bad value no longer makes `GET /api/alerts` or `/api/agents` fail for
  every user. The agent, metrics and agent-metrics lists use the same
  serialisation, and so do the alert and report webhook payloads (#147,
  part of #56).
- Agent `--server`, `--enrollment` and `--server-key` overrides are no
  longer written to the config file when the agent saves it after
  enrollment (#150). A `--server-key` override no longer fails enrollment
  against the file's old fingerprint pin.
- Disk usage now matches `df`: `free_bytes` is the space available to
  unprivileged writers, and root-reserved blocks count as used (#158).
- Encoding a frame with an empty payload no longer passes a null pointer to
  libcbor (undefined behaviour reported by UBSan on every heartbeat) (#162).
- The store is now safe to share between threads. `make_store` returns the
  backend wrapped in `SynchronizedStore`, which holds one mutex for every
  call, so HTTP workers, the data-plane thread and the status engine no
  longer interleave statements or transactions on the single SQLite or
  PostgreSQL connection (#73).
- Group assignments on PostgreSQL (`set_agent_groups`, `set_user_groups`) are
  transactional: a failed insert, such as an unknown group id, no longer
  leaves the agent or user with no groups. The configure-modal save
  (`POST /api/agents/:id/collector_config`) is one transaction through the
  new `save_agent_configuration`, so a failure saves nothing (#72).
- SQLite reads no longer report a database error as "no rows": a locked or
  failing database makes `get_session`, `is_silenced`,
  `get_offline_unalerted_agent_ids` and the list calls throw instead of
  returning an empty result (an active silence could be ignored). The
  connection now waits up to 5 seconds for a competing lock (#78).
- Bulk acknowledge and bulk delete of alerts run in one transaction on both
  backends, and the dead-agent alert is created with an atomic
  insert-if-absent (#79). On PostgreSQL this runs under a per-agent advisory
  lock, so two server nodes cannot create duplicates once every node runs
  0.9.0. A pre-0.9.0 node does not take the lock: during a rolling upgrade,
  upgrade every node before running them concurrently, or accept that one
  duplicate Heartbeat alert per agent is possible in that window.
- A heartbeat or an API setting no longer rewrites the whole agent row from a
  stale snapshot, which silently reverted a collector config, heartbeat
  interval, runbook or maintenance change saved at the same moment. The data
  plane and API now use narrow setters (`touch_agent`,
  `set_agent_collection_interval`, `set_agent_heartbeat_interval`,
  `set_agent_process_limit`); `upsert_agent` is for enrollment (#51).
- The store owns its database handles with RAII (`sqlite::Connection`,
  `postgres::Connection`), so a constructor that throws no longer leaks the
  connection; prepared statements are movable and transactions use a guard
  that rolls back without masking the original error (part of #100).
- The Windows installer registers the server service with
  `TheWatcherServer.conf`, the file the server actually reads, instead of a
  non-existent `server.json`; the docs name the same file and
  `OFFLINE_AFTER_SECONDS` key (#131). On upgrade the installer stops the
  services (aborting if one cannot be stopped), copies a KEY=VALUE
  `server.json` whose `SERVER_PUBLIC_KEY` and `SERVER_SECRET_KEY` are both
  40-character Z85 keys to `TheWatcherServer.conf` without overwriting an
  existing one, and re-registers the service, so agents keep the same server
  identity. A `server.json` that cannot be converted (either key missing,
  empty or malformed), with no `TheWatcherServer.conf` beside it, makes the
  installer refuse to run before it stops or changes anything, so the server
  never starts without its keypair or without CURVE. The copy is checked again
  and removed if it is incomplete. Config preparation and migration run in
  `PrepareToInstall` and abort the install on failure. Services that were
  running before the upgrade are started again in every outcome: on a
  preparation failure, when the file copy fails or is cancelled, and on the
  new registrations once the install finishes (installers used to leave them
  stopped until they were started by hand or the host rebooted). Service
  registration checks exit codes and retries while the old service is marked
  for deletion; a registration or restart failure shows what to run by hand
  and makes setup exit with code 10 (#131).
- Disk collector (Linux): LVM/device-mapper volumes now report I/O rates
  (`/dev/mapper/<name>` is mapped to its `dm-N` node, and symlinks such as
  `/dev/disk/by-uuid/...` are resolved); loop, snap (squashfs), pseudo and
  bind-mounted duplicates are no longer reported; octal escapes such as
  `\040` in mount paths are decoded so those disks are no longer dropped
  (#96).
- Disk collector (Linux): loop-device filtering, de-duplication and the
  `/proc/diskstats` lookup now all use the device resolved once through
  symlinks, so a `/dev/disk/by-id/...` alias of a loop device is excluded and
  two aliases of one volume are reported once; the reported `device` stays the
  string from the mount table (#96).
- Disk collector (BSD): every real mount is reported through `getmntinfo`
  instead of only `/` (FreeBSD, OpenBSD, NetBSD; not yet compiled on
  OpenBSD or NetBSD) (#93, part of).
- Process collector: a watch longer than 15 characters now matches on Linux
  through the executable basename (`/proc/<pid>/exe`, falling back to
  `argv[0]`); matching is case-insensitive on Windows; a matched process is
  reported under the watch name (#95).
- Process CPU is now percent of one core on every OS (Windows was divided by
  the core count, and Linux/BSD values were clamped to 100); a process using
  two cores reports about 200 (#95).
- Windows process collection opens each process with
  `PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ`, the rights
  `GetProcessMemoryInfo` documents. If that is denied (protected processes,
  other users' processes) it reopens with `PROCESS_QUERY_LIMITED_INFORMATION`
  only: the process stays listed with its name and CPU and its memory is
  reported as 0 (#95).
- The Toolhelp snapshot, process handles and the Linux `/proc` `DIR*` are now
  RAII-owned (#100, part of).
- Process collector (BSD): FreeBSD owner comes from the uid, not the login
  name; OpenBSD (`struct kinfo_proc`, `p_*`) and NetBSD (`kinfo_proc2`) now
  have their own readers instead of the FreeBSD layout (#93).
- The CPU collector reports a real usage value on its first `update()` and
  after a collector restart. The constructor takes a baseline and the first
  update waits until it is at least 100 ms old. Calls closer than 100 ms to
  the previous sample report the previous result instead of a few-tick
  delta (#97).
- Memory collector swap figures can no longer underflow. Windows reads the
  real page files (`EnumPageFilesW`) instead of the commit limit (#94).
  FreeBSD sums `vm.swap_info` instead of `vm.swap_reserved` (#159). A host
  with no swap reports 0/0/0, and every subtraction is clamped (the Linux
  `used_bytes` too).
- CPU usage is collected on FreeBSD (`kern.cp_time`, `kern.cp_times`),
  OpenBSD (`KERN_CPTIME`, `KERN_CPTIME2`) and NetBSD (`kern.cp_time`), with
  load averages from `getloadavg`. Memory is collected on OpenBSD
  (`VM_UVMEXP`) and NetBSD (`VM_UVMEXP2`). The OpenBSD and NetBSD code has
  not been compiled yet. Disk and process collectors are still open (part
  of #93).
- Added behavioural tests for the CPU and memory collectors (part of #137).
- The Linux CI build (GCC 11, `-O3 -Werror`) compiles again:
  `protocol_security_test` builds its CBOR payloads in a buffer sized up
  front instead of appending with `vector::insert`, which tripped a GCC 11
  `-Wstringop-overread` false positive.

### Changed

- Documentation: PostgreSQL is documented as an implemented backend selected
  with `DB_TYPE=postgres` / `POSTGRES_DSN` (build-time support required), the
  HA guide uses the real `TheWatcherServer.conf` KEY=VALUE format with its
  failover and single-writer caveats, and architecture, configuration, README
  and tracker match (#130).
- Documentation: the new-collector checklists point at the real files
  (`common/metrics.hpp` for the struct and CBOR, `server/api_json.hpp` for the
  JSON mapping), and the nonexistent `dashboard_tests/` directory and
  temperature collector references are gone (#133).
- Agent and server metric log lines no longer print a `temperatures=` count
  (#133).
- The server config save now uses the shared `common::write_private_file_atomic`
  helper (atomic, owner-only write) that the agent config also uses; the
  duplicated code in `server/config.cpp` is removed. No behaviour change (#149).
- The whole C++ tree is formatted with the root `.clang-format`, and CI
  fails any PR whose C++ is not (`clang-format` 22.1.5, pinned). The
  formatting commit is listed in `.git-blame-ignore-revs`. A new include
  category keeps `<windows.h>`/`<winsock2.h>` ahead of the Win32 headers
  that need them (#121). The CI job installs `clang-format` with
  `--only-binary :all:`, so no source-distribution setup script can run.
  The CI workflow limits `GITHUB_TOKEN` to `contents: read`.
- SonarCloud leaves test code out of duplication detection
  (`.sonarcloud.properties`); BDD scenarios repeat their setup on purpose.
  Test files are still analysed for bugs and vulnerabilities.
- CI runs for every pull request, not only those targeting `main`, so
  stacked PRs are built and tested too (#119).
- `.gitignore` covers local build directories (`build-*/`, `builddir-*/`),
  build archives (`*.zip`), nested review clones (`.codex-*/`) and
  `.serena/`. `plans/` is no longer ignored, matching the tracked plan
  history. `*.cmd`/`*.bat` are checked out with CRLF (#128).
- Version bumped to 0.9.0 in `meson.build`, `dashboard/package.json` and
  the Windows installer.
- Dashboard: `main.tsx` is split into `App.tsx`, `pages/`, `components/`,
  `hooks/` and `permissions.ts` (role helpers, now unit tested); inline
  styles moved into `styles.css`. No behaviour change (#117).
- Dashboard: removed the unused API helpers `deleteAlert` (identical to
  `archiveAlert`), `setAgentGroups`, `setAgentRunbook`, `setAgentInterval`,
  `setAgentProcessLimit`, `setAgentThresholds` and `fetchUptimeReport`, and
  the unused imports in `main.tsx` (#116).
- `docs/dashboard.md` now describes the UI as it behaves, including the
  Silences, Maintenance and Settings pages, the fixed maintenance reason and
  the first-group approval, and that the server does not serve the bundle
  (#135).

### Added

- Dashboard: ESLint (flat config, `typescript-eslint`, `react-hooks`) with
  `npm run lint`, and a `dashboard` CI job running
  `npm ci --ignore-scripts` (no package install scripts), lint, build, tests
  and `npm audit --omit=dev` (#120).
- Dashboard: the footer shows the dashboard version from `package.json`
  (#127).

## [0.8.0] - 2026-11-02

### Security

This release closes the critical and high-severity findings from the 0.7.6
security review. Several fixes change operator-visible behaviour — the
entries below call those out explicitly.

- Fixed an unauthenticated remote crash of the server: a multipart message
  on the enrollment socket left the REP state machine mid-request and the
  resulting EFSM exception killed the process. Multipart requests are now
  drained and answered with an error, every handler has a catch-all so a
  reply is always attempted, and enrollment receives are non-blocking so a
  malformed envelope can no longer wedge the data-plane thread (SEC-01).
- All ZMQ sockets now cap inbound message size (`ZMQ_MAXMSGSIZE` — 64 KiB
  enrollment, 4 MiB data plane) and bound the connection handshake
  (`ZMQ_HANDSHAKE_IVL`); the enrollment reply uses a send timeout
  (SEC-02).
- CBOR payloads are structurally pre-validated before decoding: nesting
  depth is capped and declared definite-container sizes must fit the
  remaining bytes, so a 9-byte payload can no longer demand a
  multi-gigabyte allocation or overflow the stack on free (SEC-03).
- The data-plane agent identity is now bound to the CURVE key: ZAP reports
  the agent_id each approved key belongs to, every frame's claimed
  `agent_id` must match it, command ACKs are accepted only from the agent
  a command was dispatched to, and config replies are routed to the actual
  peer identity (SEC-04, SEC-41).
- Enrollment validates all fields (agent_id charset and length, hostname
  and platform bounds), rate-limits per agent_id and per source address
  with pruned tracking maps, caps the pending-approval queue, sanitises
  control characters before logging, and supports an optional pre-shared
  `ENROLLMENT_TOKEN`. A pending record's public key is immutable — a
  different key no longer overwrites it before approval — and re-enrolling
  an approved agent no longer refreshes its `last_seen` (which could
  suppress dead-agent detection) (SEC-05, SEC-06).
- Metric rows, evaluation times, alert timestamps and retention pruning now
  run on the server's receive clock; an agent-supplied future timestamp can
  no longer delete other agents' history (SEC-07).
- **Behaviour change:** agent enrollment fails closed. A reply contradicting
  a configured server key is rejected, and an approved reply without a
  server key pin is a hard error unless the operator passes `--insecure`
  (or sets `ALLOW_INSECURE=1`). `--insecure` applies to that process only
  and is never written to the config file; `--install-service --insecure`
  forwards it to the Windows service command line. The data plane never
  silently connects without CURVE any more (SEC-08).
- The server still accepts the 4-element enrollment request sent by 0.7.x
  agents (no enrollment token), so existing agents keep enrolling after a
  server upgrade. When `ENROLLMENT_TOKEN` is set, those requests are
  rejected because the token is missing (SEC-05).
- A CURVE public key can belong to one agent only: enrollment refuses a key
  that another agent already holds, and approval refuses a key that is
  already bound to another approved agent (HTTP 409). Data-plane log lines
  sanitise the peer-supplied `agent_id` and command ACK text (SEC-04,
  SEC-05).
- **Behaviour change:** approval and revocation are enforced per frame on
  the data plane — rejected, deleted or still-pending agents' frames are
  dropped immediately instead of being honoured on existing sessions
  (SEC-09).
- **Behaviour change:** log monitoring is now restricted end to end. The
  server validates log monitor entries and only global admins may configure
  log paths; on the agent, log monitoring is disabled until the operator
  sets `LOG_ALLOWED_DIRS`, and reads are additionally capped (line length,
  bytes and matches per tick) and refuse anything but regular files inside
  the allowlist. Each file is opened once without following a final
  symlink and without blocking on FIFOs; all checks and reads use that
  descriptor, and the opened file's real location is re-checked against
  the allowlist where the OS reports it. Only newline-terminated lines are
  matched, so a line still being written is matched once it is complete.
  Operators may resubmit an agent's collector config with the existing log
  monitors unchanged; adding, changing or removing log monitors needs a
  global admin (SEC-10).
- Per-agent history, agent-scoped runbooks and view `agent_ids` now enforce
  group scoping like the rest of the per-agent API. Alerts attach only the
  alerting agent's own runbook or a generic one, never a runbook scoped to
  another agent (SEC-11).
- Login rate limiting now uses a fixed window that resets on expiry — the
  lockout re-arms instead of allowing unlimited guessing after the first
  15 minutes. The per-account lockout is keyed by client address and
  username, so one source can no longer lock an account for everyone; a
  per-source-address throttle covers all usernames, and an account-wide
  ceiling of 50 failures limits distributed guessing. New `TRUSTED_PROXIES`
  server setting: X-Forwarded-For / X-Real-IP are used for the client
  address only when the connection comes from a listed proxy (SEC-12).
- PostgreSQL deployments: `get_session` now filters disabled users, and
  `disable_user`/`delete_user` carry the same `built_in` guard as SQLite,
  so the bootstrap admin cannot be deleted and recreated with the public
  default password. Both backends now report affected rows and the API
  returns 404 for unknown or built-in ids instead of `{"ok":true}`
  (SEC-13, SEC-14).
- The server config is written atomically: a uniquely named owner-only
  temp file is written and renamed over the target (on Windows, replaced
  without deleting the old file first). Only directories that the save
  creates are restricted to the owner; existing directories are left
  unchanged. The load path warns when an existing config is group/world
  readable (SEC-15).
- Bumped cpp-httplib from 0.43.2 to 0.63.0, which upstream fixed several
  server advisories in (CRLF injection via header parsing, X-Forwarded-For
  crash under trusted proxy configs, negative chunk size DoS, trailer
  header CRLF injection) (SEC-44).
- **Behaviour change (Windows):** the installer no longer grants
  `Users:modify` on the config directory; it now resets the ACL to SYSTEM
  and Administrators only, removes the Users, Authenticated Users and
  Everyone grants left by older installers, resets existing files to
  inherit that ACL, and aborts the install if the ACL cannot be applied or
  verified. Existing installs keep the old ACL until the 0.8.0 installer is
  re-run (SEC-45).

### Fixed

- The data-plane ROUTER drops a message with fewer parts than
  `[identity][empty][payload]` instead of blocking on a receive for the
  missing part, which stalled enrollment, command dispatch and offline
  checks until the next message arrived.
- The API server binds its port before `start()` returns, so the first
  request after startup no longer races the listener. If the bind fails,
  server startup now fails instead of reporting a REST API that is not
  listening.

### Changed

- Bumped project version to 0.8.0.

## [0.7.6] - 2026-05-18

### Added
- Added `docs/01-progress-tracker.md` as the consolidated alpha, beta, and v1
  production release progress tracker with explicit Done and TODO sections.

## [0.7.5] - 2026-05-14

### Changed
- Added GitHub Actions job-summary download links for archived Linux,
  Windows, and FreeBSD build output artifacts.

## [0.7.4] - 2026-05-14

### Changed
- Raised first-party Meson builds to `warning_level=3` with `werror=true`
  while explicitly keeping third-party fallback dependencies outside that
  warnings-as-errors policy.

## [0.7.3] - 2026-05-14

### Changed
- Moved Pending Enrollments into the top of the Agents page and reordered the
  top navigation to Monitoring, Views, Alerts, Silences, Maintenance, Agents,
  Users, and Settings.
- Renamed dashboard user role presentation to account Type labels so account
  type is visually distinct from group membership.
- Tightened group-scoped API responses so group accounts only receive their
  assigned groups and group-owned views, maintenance windows, and silences.
- Alert rows now open the alerting agent's markdown runbook in a rendered
  modal instead of showing raw markdown or linking to legacy runbook URLs.
- The agent Configure modal now closes after a successful save, includes
  tooltips on every configurable field, and stacks Disk/Network enablement,
  threshold, and anomaly controls vertically.
- Moved the Agent Management Configure action between Commands and Delete.

## [0.7.2] - 2026-05-14

### Fixed
- Fixed Linux/BSD integration-test startup timing by waiting until the REST API
  accepts a session request before asserting the unauthenticated response.
- Normalized browser-origin checks to compare scheme, host, and effective
  default ports so HTTPS reverse-proxy requests are not rejected when one side
  omits `:443`, while HTTP origins on HTTPS API hosts remain blocked.
- Hardened webhook IPv6 literal filtering by parsing IPv6 addresses before
  rejecting loopback, unspecified, documentation, benchmark, multicast,
  link-local, unique-local, and IPv4-mapped restricted targets.
- Filtered Windows network collection to IP Helper adapters that correspond to
  `ipconfig` output instead of lower-layer NDIS, bridge, tunnel, scheduler, and
  filter devices.

## [0.7.1] - 2026-05-14

### Fixed
- Fixed pending enrollment Approve and Reject actions from dashboard origins
  that share the API host or loopback machine but use a different port. The API
  now emits credentialed CORS headers for those allowed origins while continuing
  to reject arbitrary cross-site unsafe requests.

## [0.7.0] - 2026-05-14

### Added
- Added `docs/threat-model.md` covering assets, trust boundaries, STRIDE-style
  threats, mitigations, and residual risks for the dashboard, API, agents,
  database, configuration files, and outbound webhooks.
- Added direct BDD coverage for webhook URL validation, oversized config input,
  and cross-origin authenticated browser write rejection.

### Changed
- Hardened REST API responses with baseline browser security headers and reject
  unsafe authenticated requests when `Origin` or `Referer` does not match
  `Host`.
- Replaced duplicated alert/report webhook URL parsing with a shared validator
  that rejects localhost, private, link-local, multicast, malformed authority,
  userinfo, invalid-port, and non-HTTP targets before making outbound requests.
- Bounded server and agent KEY=VALUE config parsing to reject files above 1 MiB
  or individual lines above 8 KiB before applying settings.

## [0.6.0] - 2026-05-13

### Added
- Added the six-role access model: `global_admin`, `global_operator`,
  `global_viewer`, `group_admin`, `group_operator`, and `group_viewer`.
  Global roles can see all agents, alerts, views, and users according to their
  write permissions; group roles are scoped to their assigned groups.
- Added group-scoped views. View create/edit now accepts `group_id`, and
  non-global users only see views owned by or assigned to their visible groups.
- Added per-agent heartbeat configuration to the dashboard Configure modal,
  API, persisted agent record, and `CONFIG_UPDATE` payload. Valid values are
  1 through 60 seconds, with a default of 5 seconds.
- Added per-agent markdown runbooks to the agent Configure modal and alert
  payloads so alerts can show host-specific response guidance.
- Added IP address display on Monitoring, Agents, and Pending Enrollment rows.
  The agent id remains visible on Agents and Pending Enrollment rows.
- Added Windows network collector support through IP Helper APIs.

### Changed
- Moved approved-agent group assignment into the agent Configure modal.
- Operators can configure maintenance windows for agents they can operate.
- Alerts, agents, and views are filtered by group membership for non-global
  users. Moving an agent between groups moves its alert visibility with it.
- User and group management now follows the new role model: global admins can
  manage users and groups, global/group operators have read-only user access,
  and group admins can create users only inside their own groups.
- Legacy global runbook create/delete endpoints now require `global_admin`
  because those runbooks are consumed globally by alert generation.
- Linux and BSD foreground agents now respond to `Ctrl+C` while waiting for
  enrollment retry loops.

### Removed
- Removed the agent temperature collector from the build and dashboard health
  model. Historical temperature metric fields remain in the wire schema for
  backward compatibility.

### Removed
- **Bazel/Bazelisk build system removed.** Meson + Ninja is now the sole
  supported build path on Windows, Linux, and BSD. Deleted `BUILD.bazel`,
  `MODULE.bazel(.lock)`, `WORKSPACE.bazel`, `.bazelrc`, `.bazelignore`,
  every per-package `BUILD.bazel`, `scripts/bazel.cmd`, `build.cmd`, the
  Bazel-only `patches/` and `third_party/` directories, and the Bazel
  documentation. `scripts/setup-build-env.ps1` no longer installs Bazelisk
  or writes `.bazelrc.user`; the Windows quick start now goes through
  `meson-build.cmd`, and binaries land in `builddir-release/{server,agent}/`
  instead of `bazel-bin/`.

## [0.5.0] - 2026-05-11

### Added
- **Runbook links** (Issue #11): a `RunbookRecord` maps `(indicator, status)`
  pairs to a URL and optional notes. A wildcard indicator `"*"` matches any
  indicator when no exact match exists. Runbooks are stored in a new `runbooks`
  table (SQLite and PostgreSQL). When an alert fires, `StatusEngine` looks up a
  matching runbook and embeds the URL in the `AlertRecord`; the URL is also
  included in the webhook payload. New API endpoints:
  - `GET /api/runbooks` — list all runbooks (viewer+)
  - `POST /api/runbooks` — create a runbook (`{indicator, status, url, notes}`; admin only)
  - `DELETE /api/runbooks/:id` — delete a runbook (admin only)
  The dashboard gains a **Runbooks** management tab (admin only) and alert rows
  link to the runbook URL when one is present.
- **PostgreSQL backend** (Issue #12): `PostgresStore` is a full implementation
  of the `Store` interface using libpq (the PostgreSQL C client library).
  Enabled at build time when `libpq` is found by Meson; the
  `HAVE_LIBPQ` preprocessor flag gates all PostgreSQL code. Activate by
  setting `db_type = "postgres"` and `postgres_dsn = "..."` in the server
  config. The schema uses `BIGSERIAL`, `BOOLEAN`, `DOUBLE PRECISION`, and
  `BYTEA` PostgreSQL types; BYTEA is stored as `\x`-prefixed hex strings in
  text mode. `make_store()` routes the connection automatically.
  A dedicated `store_postgres_test` test executable covers agent CRUD,
  metrics, alerts, runbooks, settings, sessions, maintenance windows, and
  silence rules against a live PostgreSQL database.
- **HA topology documentation** (`docs/ha-topology.md`): reference
  two-server active/standby setup with pgBouncer connection pooling and
  keepalived VRRP for virtual-IP failover.

## [0.4.1] - 2026-05-07

### Added
- **Anomaly detection** (Issue #6): when `multiplier > 0` is set in the
  collector config for CPU, memory, disk, or network, the status engine
  computes a rolling baseline mean from historical metrics and flags the
  indicator Yellow if the current sample exceeds `mean × multiplier`. A
  5-minute in-process cache avoids recomputing on every reading. Config is
  stored as two new CBOR fields (`cpu_anomaly`, `memory_anomaly`) on
  `CollectorConfig` and per-disk/network entries, with backward-compatible
  size checks so old agents receive old configs without errors. The
  dashboard collector-config editor exposes the multiplier and baseline
  window fields.
- **Scheduled fleet digest reports** (Issue #7): a report scheduler sends
  a JSON health digest to a configured webhook URL on a daily or weekly
  cadence (UTC hour, optional day-of-week). The digest includes agent
  totals, online/offline/maintenance counts, alert counts by severity, and
  the top-5 agents ranked by oldest unacknowledged alert.
  `GET/PUT /api/settings` gains `reports_enabled`, `reports_schedule`,
  `reports_hour`, `reports_day_of_week`, and `reports_webhook_url` fields.
  `POST /api/reports/send` triggers an immediate digest (admin only). The
  Settings page gains a Scheduled Reports section.
- **Stale metrics detection** (Issue #8): when `stale_after_seconds > 0`
  in the collector config, the status engine tracks the last observed value
  per agent/indicator; if it remains unchanged beyond the window (epsilon
  0.01) the indicator is raised to at least Yellow. The staleness clock
  resets automatically when the value next changes. Defaults to 0
  (disabled). Carried as CBOR element 12 in the 13-element
  `CollectorConfig` array with a backward-compatible decode guard.
- **Log file tailing and alerting** (Issue #9): agents can now watch log
  files with regex patterns and forward matched lines to the server as
  `LOG_MATCH` frames (new frame type `0x09`). `LogMonitorConfig`
  `{path, pattern, indicator_name, severity, enabled}` is added to
  `CollectorConfig` (CBOR element 13). `LogCollector` polls files each
  collection cycle, tracks file position and inode for rotation detection,
  and matches lines with `std::regex`. The server inserts matches into a
  `log_matches` table and raises an alert on the named indicator (silence
  rules respected). `GET /api/agents/:id/log-matches?limit=N` returns the
  match history. The dashboard gains a Log Watches config section and a Log
  Matches history card in the agent detail panel.
- **Custom dashboard views** (Issue #10): operators can create named views
  that show a chosen subset of agents. Views can be public (visible to all
  users) or private (owner only). `GET/POST /api/views` and
  `GET/PUT/DELETE /api/views/:id` manage views; the dashboard adds a Views
  tab with bookmarkable `#view-{id}` URLs so the monitoring panel can be
  filtered to a specific view.

### Fixed
- **LogCollector incremental tailing**: after `std::getline` exhausts a
  file it sets `eofbit`, causing the subsequent `tellg()` to return `-1`.
  The collector now calls `file.clear()` before `tellg()` and falls back to
  `st_size` if the position is still negative. Previously every incremental
  tick after the first silently re-read from offset `-1`, causing repeated
  duplicate matches.
- **Integration test enrollment re-check**: the agent lifecycle scenario
  re-sent a ZMQ enrollment request immediately after HTTP approval, which
  was blocked by the 10-second per-agent rate limiter. Replaced with an
  HTTP-API approval check; the subsequent CURVE handshake proves key
  exchange.

### Tests
- **BDD store coverage** (12 new scenarios, 62 new assertions): inclusive
  `get_metrics_in_window` boundaries, session expiry edge cases
  (`expires_at > now_ms`), agent deletion cascades, silence rule expiry at
  exact boundary, `approve_agent` clearing a rejected flag, and several
  other state-transition edge cases.
- **BDD status engine coverage** (9 new scenarios): `classify_percent`
  exact thresholds (80/90/95), direct green→red CPU jump, process-scope
  isolation, `process_readings=1` immediate red, maintenance exit
  behaviour, NaN→grey, anomaly firing and suppression (< 10 baseline
  samples), stale metric detection.
- **New test suite — LogCollector** (`agent_tests/log_collector_test.cpp`,
  11 scenarios / 29 assertions): empty config, missing file, empty file,
  pattern match/no-match, `take_matches` drain, incremental tailing,
  file-rotation detection, disabled config, invalid regex, stale-state
  pruning via `set_configs`.
- **Integration tests — API coverage** (6 new scenarios / 83 new
  assertions): user management (create/disable/enable/password change/
  delete), group management (create/list/validation), views CRUD with
  public/private visibility rules, alert listing and bulk operations,
  session and login API, enrollment rejection and per-agent config APIs
  (description, thresholds, uptime, log-matches, agent deletion).

## [0.4.0] - 2026-05-06

### Added
- **Alert silencing**: operators can create silence rules scoped to an agent
  and/or indicator with an expiry time; silenced transitions do not generate
  alerts.
- **Metrics retention**: configurable retention period (default 30 days) prunes
  old metric rows automatically; configurable via the settings API.
- **Status history viewer**: `GET /api/agents/:id/history` returns the
  per-indicator transition log; surfaced in the dashboard alongside each agent.
- **Bulk alert operations**: `POST /api/alerts/bulk-ack` and
  `POST /api/alerts/bulk-archive` let operators acknowledge or archive multiple
  alerts in a single request.
- **Maintenance windows**: scheduled maintenance windows (with optional wildcard
  agent matching) automatically put agents into maintenance mode and resume them
  when the window ends; manageable via the settings panel.

### Security
- Enrollment channel: public key is now locked after first approval — subsequent
  re-enrollments from the same agent ID cannot replace an approved key (prevents
  identity hijacking via the unauthenticated enrollment socket).
- Enrollment channel: per-agent-ID rate limit of one request per 10 seconds.
- Enrollment channel: z85 public key format validated (exactly 40 chars, z85
  alphabet) before storage; malformed keys are rejected.
- Login endpoint: rate-limited to 5 failures per username; account locked for
  15 minutes on breach, with `HTTP 429` + `Retry-After: 900` response.
- Command IDs: replaced truncated 32-bit millisecond timestamps with
  8-byte cryptographically random hex values (via libsodium) to eliminate
  collision risk.
- Command dispatch: `command_type` validated against the known `CommandType`
  enum; `args` payload capped at 4096 bytes.
- Dispatched command map: entries are evicted after a 5-minute TTL to prevent
  unbounded memory growth when agents drop without ACKing.
- Webhook SSRF: loopback (`127.x`, `::1`, `localhost`), RFC-1918 (`10.x`,
  `172.16–31.x`, `192.168.x`), and link-local (`169.254.x`) hosts are blocked
  in the webhook URL validator.
- Session cookie: `Secure` flag added to both login and logout `Set-Cookie`
  headers.
- Input length caps on all user-supplied string fields stored to the database
  (username ≤ 64, password ≤ 256, note/description ≤ 4096, reason ≤ 1024,
  group name 1–64); over-length inputs return `HTTP 400`.
- Metrics history `limit` query parameter capped server-side at 1000.
- `column_exists()` validates table/column identifiers against `[a-z_]` before
  SQL string concatenation.
- Agent config file (`TheWatcherAgent.conf`) permissions set to `0600` on POSIX
  after every write, protecting the embedded CURVE secret key.
- All mutating API endpoints check `Content-Type: application/json`; requests
  with a mismatched type return `HTTP 415`.
- `gethostname()` / `GetComputerNameA()` return values checked; falls back to
  `"unknown"` on failure.
- Login form no longer pre-fills the default admin username.
- Added code comments documenting the ZAP NULL-mechanism allowance rationale
  and the plain-HTTP deployment requirement for the REST API.

## [0.3.3] - 2026-05-03

### Changed
- `meson-build.cmd` now configures, builds, and tests the static Meson release
  build in `builddir-release`.
- Ignored the generated release Meson build directory for Git and Bazel package
  discovery.

## [0.3.2] - 2026-05-03

### Changed
- Meson now builds libzmq and libsodium as local static subprojects from
  source archives. The Meson build no longer requires vcpkg, pkg-config, or
  system ZeroMQ/libsodium packages.
- Updated the Meson helper script to configure static local dependency builds
  and reconfigure an existing build directory without vcpkg assumptions.
- Kept Bazel aligned with the same static ZeroMQ/libsodium model by preserving
  archive-backed libzmq, libsodium, and cppzmq wrappers for Windows, Linux, and
  BSD builds.

### Fixed
- Patched libzmq's sodium detection for parent-supplied libsodium include and
  library paths so Meson and Bazel can both build CURVE-enabled static ZeroMQ
  without external package manager metadata.
- Ignored generated Meson build and subproject extraction directories from
  Bazel package discovery so local Meson smoke builds do not break
  `bazel test //...`.

## [0.3.1] - 2026-05-03

### Changed
- Kept ZeroMQ on the static Bazel path and removed vcpkg project hooks from the
  Windows/Meson helper flow. `build.cmd` now relies on the archive-backed static
  `libzmq` build with libsodium/CURVE support.
- `common/crypto.hpp` now owns Z85 encode/decode for Curve25519 keys instead of
  calling ZeroMQ's C helper functions. Config/protocol-only targets can link
  shared crypto code without forcing a ZeroMQ build.

### Added
- Added BDD coverage for the local Z85 encode/decode path and invalid key
  rejection.

## [0.3.0] - 2026-05-03

### Changed
- Replaced MessagePack with libcbor 0.13.0 for every ZeroMQ wire frame
  (Frame, EnrollRequest, EnrollResponse, CommandMessage, AckMessage,
  SetIntervalArgs, SetProcessLimitArgs, ConfigUpdate, SystemMetrics,
  CollectorConfig). The agent and server now exchange CBOR exclusively;
  the `msgpack-cxx` dependency is gone.
- Replaced JSON config files with KEY=VALUE format. Agent config moved from
  `agent.json` to `TheWatcherAgent.conf`; server config from `server.json`
  to `TheWatcherServer.conf`. Comments use `#`. The legacy JSON load path
  in the agent has been removed; existing 0.2.x JSON configs must be
  re-generated by deleting the old file and letting the agent recreate it.
- SQLite metrics column changed from `metrics_json TEXT` to
  `metrics_cbor BLOB`. The server stores the original CBOR payload from
  the agent verbatim; the REST API decodes CBOR → SystemMetrics → JSON
  for the dashboard so the API shape is unchanged. Existing 0.2.x
  databases auto-migrate by dropping and recreating the metrics table on
  first start (historical metrics are lost; agents will repopulate).
- Wire `to_cbor`/`from_cbor` are now templates with `template<>`
  specialisations (matching the `from_cbor` pattern that already worked).
  This removes a two-phase qualified-name-lookup gotcha that the previous
  overload set hit when called via `pack<T>()`.
- libcbor's `cbor_get_uint64()` reads 8 raw bytes regardless of the stored
  width; we wrap it in a width-aware `cbor_uint_value()` helper. Without
  this, decoding any small uint8/16/32 returned garbage adjacent memory.

### Added
- Meson build system added side-by-side with Bazel. Top-level `meson.build`,
  per-component `meson.build` files (common, agent, agent/collectors,
  server, common_tests, agent_tests, server_tests, integration_tests).
  Subprojects: catch2, sqlite3, nlohmann_json, cpp-httplib, libcbor,
  cppzmq, libzmq, and libsodium. See docs/build-environment.md.
- New `server/api_json.hpp` provides nlohmann adapters for the metric
  structs so the REST API can keep emitting JSON without dragging
  nlohmann into `common/`.

### Fixed
- libcbor Bazel BUILD now lists every C source the library depends on (the
  previous list was missing `internal/builder_callbacks.c`,
  `internal/encoders.c`, `internal/loaders.c`, `internal/memory_utils.c`,
  `internal/stack.c`, `internal/unicode.c`).
- `third_party/libcbor/generated/cbor/configuration.h` now defines
  `CBOR_MAX_STACK_SIZE` (was missing; broke compilation of `internal/stack.c`).
- Windows include order across collectors and agent/server config files —
  `<windows.h>` must precede `<sysinfoapi.h>`, `<shlobj.h>`, `<psapi.h>`,
  `<sddl.h>`, `<tlhelp32.h>` to set up the architecture macros that
  `<winnt.h>` depends on.
- `to_cbor_vector` / `config_to_cbor_vector` template definitions reordered
  to come AFTER the per-struct `to_cbor` specialisations they dispatch to.

### Removed
- `nlohmann/json.hpp` is no longer included by `common/metrics.hpp`,
  `agent/config.hpp`, `agent/config.cpp`, `server/config.hpp`,
  `server/config.cpp`, or `server/server.cpp`. nlohmann remains in
  `common/collector_config.hpp` (REST round-trip) and `server/api*` (REST
  output) only.

## [0.2.29] - 2026-05-01

### Added
- Added persisted per-agent collector configuration for absolute CPU/memory
  thresholds, per-fixed-disk thresholds, per-interface network Mbps thresholds,
  process watches, and per-collector consecutive-reading counts.
- Added an Agents Configure modal that replaces inline threshold controls and
  sends the full collector config to the server.

### Changed
- Status evaluation now uses absolute collector thresholds instead of
  percentage-of-average settings.
- Process monitoring now checks exact executable names and expected counts,
  escalating missing processes across consecutive failed readings.

## [0.2.28] - 2026-05-01

### Changed
- Changed Monitoring alert notifications to render each alert as a separate
  severity-themed item using the known hostname with the agent id as secondary
  text.

### Fixed
- Fixed the Monitoring table heading so it no longer floats over row values
  when alert notifications are visible.

## [0.2.27] - 2026-05-01

### Added
- Added per-agent CPU, memory, disk, and network warning/degraded/critical
  percentage thresholds with API and Agent Management controls.
- Added hostname-plus-agent-id alert display so Alerts matches the Monitoring,
  Agent Management, and Pending Enrollment identity presentation.

### Changed
- Status evaluation now uses per-agent threshold overrides before falling back
  to global threshold settings, while retaining the absolute 70/85/95 caps.

## [0.2.26] - 2026-05-01

### Added
- Added the server CURVE public key and a pinned BLAKE2b-256 fingerprint to
  approved enrollment responses.
- Added agent-side server key pinning so agents persist the approved server key
  fingerprint and reject later enrollment approvals from a different server key.

### Changed
- Agents can now learn the server public key during approval, removing the need
  to pre-populate `SERVER_PUBLIC_KEY` for normal first enrollment.

## [0.2.25] - 2026-05-01

### Added
- Reintroduced the dashboard heartbeat indicator as the final Monitoring dot.
- Added agent metric collection summaries, outgoing frame send diagnostics, and
  server-side inbound metric/heartbeat receive summaries.

### Changed
- Renamed the dashboard Overview page to Monitoring in the operator UI and
  documentation.

## [0.2.24] - 2026-05-01

### Changed
- Changed SingleLog file flushing so trace, debug, and info entries flush every
  100 file log lines while notice, warning, error, and critical entries flush
  immediately.

## [0.2.23] - 2026-05-01

### Added
- Added grouped Overview rendering so approved agents are listed under their
  assigned group names, with an Overview group filter and an Ungrouped section.
- Added collector-level trace/debug logging for CPU, memory, disk, network,
  process, and temperature collection updates.
- Added BDD coverage for Overview grouping/filtering and live SingleLog file
  flushing.

### Changed
- SingleLog now periodically flushes file output so agent log files show
  INFO/DEBUG/TRACE records while the agent is still running.

## [0.2.22] - 2026-05-01

### Fixed
- Fixed an API route lifetime bug where callbacks captured stack-local helper
  lambdas from route setup, causing the server-agent integration test to crash
  with SIGSEGV on authenticated routes after login.

## [0.2.21] - 2026-05-01

### Changed
- Raised Bazel C++ compiler warnings to the highest configured warning level:
  `-Wall -Wextra -Wpedantic` for Linux, macOS, and BSD builds and `/Wall` for
  MSVC.

## [0.2.20] - 2026-05-01

### Fixed
- Updated the server-agent integration test to authenticate through
  `/api/login`, persist the returned session cookie, and query pending
  enrollments through `/api/pending-enrollments`.
- Removed the workspace root `tmp` directory created during local server
  debugging because it collides with a `rules_foreign_cc` generated file named
  `tmp` during Windows Bazel builds.

### Documentation
- Documented the Windows Bazel failure mode caused by a root-level `tmp`
  directory.

## [0.2.19] - 2026-05-01

### Added
- Added persistent operations schema for groups, users, sessions, agent-group
  membership, status history, alerts, and server settings.
- Added SQLite-backed login with the default admin account
  `thewatcher` / `look_at_me`, libsodium password hashing, and HTTP-only
  session cookies.
- Added approved-agent and pending-enrollment API separation, admin approval
  with group assignment, and RBAC gates for viewer/operator/admin actions.
- Added status transition evaluation with five-minute-average thresholds,
  worsening-only alert generation, alert acknowledgement, alert soft delete,
  and maintenance alert clearing.
- Added dashboard login, pending enrollments, alerts, users/groups, status,
  alert dot, no-data grey, amber degraded, and maintenance blue workflows.
- Added an Inno Setup Windows EXE installer script for installing server and
  agent binaries under `C:\Program Files\TheWatcher`.
- Added BDD coverage for operations persistence and status/alert transitions.

### Changed
- Split Bazel persistence/status tests away from the full server library so
  store and status engine tests do not pull the ZeroMQ build path.

## [0.2.18] - 2026-05-01

### Fixed
- Resolved relative server SQLite `db_path` values beside the active server
  config file so enrolled agents persist across restarts launched from
  different working directories.

### Added
- Added a dev-task-planner implementation plan for alert history, pending
  enrollments, RBAC, authentication, dashboard workflow changes, and Windows
  EXE installers.
- Added BDD coverage for server config database path resolution.

## [0.2.17] - 2026-05-01

### Changed
- Converted project logging call sites to use `SingleLog.hpp` `LOG_` and
  `LOGF_` macros instead of direct logging function calls.
- Configured `SingleLog` directly from each binary entry point instead of using
  a project wrapper.

### Added
- Added detailed debug and trace logging around server startup, enrollment,
  API agent actions, command dispatch, ZAP decisions, SQLite store mutations,
  agent enrollment, config loading, and service control paths.

## [0.2.16] - 2026-05-01

### Added
- Added component prerequisite requirements to the installation documentation.
- Added nginx and Apache httpd examples for publishing the dashboard on Linux
  and BSD, including production static hosting and development-only Vite
  reverse proxy setups.

## [0.2.15] - 2026-05-01

### Added
- Added complete operator documentation for installation, configuration,
  running, service operation, logging, and dashboard use.
- Added architecture and development documentation covering every major
  component and the end-to-end workflow for adding collectors.

### Changed
- Reworked the README into a documentation index and quick-start guide.
- Expanded existing build, dashboard, logging, service, and collector docs to
  match the current server and agent behavior.

## [0.2.14] - 2026-05-01

### Added
- Added `TheWatcherAgent.conf` support with `THEWATCHER_SERVER=<host>` for
  deriving agent data and enrollment endpoints.
- Added agent-originated config refresh requests after metrics submissions.
- Added server-side offline detection using configurable
  `offline_after_seconds`.

### Changed
- Persisted per-agent collection interval and process limit settings so config
  refresh responses can return current server-side settings.

## [0.2.13] - 2026-05-01

### Added
- Added a persisted maintenance state for agents so pause and resume
  acknowledgements are reflected in the dashboard.
- Added a row-level Resume action for agents that are already in maintenance.

### Changed
- Hid approve and reject controls once enrollment has been decided, leaving
  deletion as the way to start enrollment again.

### Fixed
- Fixed the agent management table header and row layout so the heading bar no
  longer overlaps the row content.

## [0.2.12] - 2026-05-01

### Added
- Added an end-to-end server-agent integration test that exercises enrollment,
  API approval, CURVE connection, real collector metrics, API-visible server
  storage, disconnect, and shutdown.
- Added server-observed agent connection state to `/api/agents`.

### Fixed
- Sent the agent disconnect ACK immediately before stopping the agent IO loop so
  the server can record the disconnect.

## [0.2.11] - 2026-05-01

### Added
- Added dashboard agent management for approving, rejecting, configuring, and
  deleting enrolled agents.
- Added persistent rejected enrollment state so rejected agents receive an
  enrollment rejection instead of remaining pending.
- Added dashboard API helpers and tests for agent management actions.

### Fixed
- Corrected pending enrollment responses so unapproved agents keep polling
  instead of treating a pending response as approved.

### Changed
- Renamed Bazel executable targets to `//server:TheWatcherServer` and
  `//agent:TheWatcherAgent`.

## [0.2.10] - 2026-04-30

### Fixed
- Enabled ZeroMQ CURVE support in the Windows libzmq build by patching sodium
  detection for Bazel-provided libsodium.
- Rebuilt the documented `//server:server` target as `server.exe` so the
  command-line server path is no longer stale.
- Added foreground startup signaling so server worker-thread failures are
  reported before printing the running message.

## [0.2.9] - 2026-04-30

### Added
- Added a common `SingleLog` wrapper and wired server, agent, API, ZAP, and
  Windows service runtime diagnostics through it.
- Added logging documentation covering default log locations, levels, and
  service-mode behavior.

## [0.2.8] - 2026-04-30

### Added
- Added the React, TypeScript, and Vite dashboard implementation using the
  supplied dark infrastructure table sample as the visual direction.
- Added dashboard health mapping, summary counts, expandable detail rows, API
  polling, and Vitest coverage for status behavior.
- Added shared Windows service helpers and wired server/agent flags for
  `--service`, `--install-service`, `--uninstall-service`, and
  `--service-name`.
- Added dashboard and service installation documentation.

### Changed
- Foreground mode remains the default for server and agent binaries.

## [0.2.7] - 2026-04-30

### Fixed
- Added the workspace-local `bazel-help-root` verification output directory to
  `.bazelignore` so `bazel test //...` does not scan generated Bazel install
  files as source packages.
- Documented the local output-root troubleshooting case.

## [0.2.6] - 2026-04-30

### Added
- Added `scripts/bazel.cmd` as the supported Windows Bazel launcher so
  `BAZEL_SH` is set before the Bazel server chooses its shell executable.

### Changed
- The setup script now persists `BAZEL_SH` in the user environment.
- Updated build documentation to use `scripts/bazel.cmd` for Windows builds.

## [0.2.5] - 2026-04-30

### Added
- Added `scripts/setup-build-env.cmd` as an execution-policy-safe launcher for
  the PowerShell build environment setup script.

### Changed
- Updated setup documentation to use the `.cmd` wrapper by default.

## [0.2.4] - 2026-04-30

### Fixed
- Added Git Bash `BAZEL_SH` and `PATH` settings to Bazel host action
  environments so `rules_foreign_cc` exec-tool builds do not fall back to the
  WSL `bash.exe` launcher.
- Updated the build environment script to write the host action settings and
  restart the Bazel server after changing `.bazelrc.user`.

## [0.2.3] - 2026-04-30

### Added
- Added `scripts/setup-build-env.ps1` to install/check Windows build tools,
  configure Bazel's Git Bash environment, and optionally provision WSL Linux
  build dependencies with Bazelisk.
- Added build environment documentation covering requirements, setup, build
  steps, WSL verification, and troubleshooting.

### Fixed
- Ensured Bazel Windows actions prefer Git Bash before the WSL `bash.exe`
  launcher in `PATH`.
- Changed the Windows libsodium version-header genrule to use `cmd.exe` instead
  of Bash so it does not fail when WSL has no installed distro.

## [0.2.2] - 2026-04-30

### Changed
- Moved straightforward third-party dependencies to Bzlmod registry modules:
  Catch2, cpp-httplib, and SQLite.
- Updated `bazel_skylib` to the selected registry version used by transitive
  dependencies.
- Updated Catch2 tests to use the BCR module's `catch2_main` target and modular
  Catch2 v3 headers.

### Fixed
- Added a Bazel 9 compatibility patch for the BCR cpp-httplib module's
  `cc_library` load.

## [0.2.1] - 2026-04-30

### Fixed
- Added Bazel 9 / Windows build fixes for `rules_foreign_cc`, Git Bash shell
  selection, and libsodium's in-place autotools configuration.
- Added a native Windows libsodium Bazel target and corrected libzmq's Windows
  static-library output name.
- Configured msgpack-cxx to use bundled predef headers instead of requiring
  Boost for endian detection.
- Included the protocol helpers in `server/api.cpp` and fixed command argument
  packing to use the declared `proto::pack` helper.
- Fixed `get_os_version()` so the Windows path does not instantiate POSIX
  `utsname`, and added BDD coverage for a non-empty OS version string.
- Defined `ZMQ_STATIC` and Windows socket link libraries in the cppzmq Bazel
  wrapper so tests link against the static libzmq build.
- Replaced Unix-style Windows `-l...` link options with MSVC `*.lib` names and
  linked `shell32.lib` from `agent_lib` for the agent config path helper.
- Linked `shell32.lib` from `server_lib` for the server config path helper.
- Aligned the agent collector contract around `Collector::name()` and
  `Collector::update(SystemMetrics&)`.
- Removed stale `collect()` / `getRates()` collector declarations and
  placeholder collector namespaces that did not match `agent/agent.cpp`.
- Updated CPU, disk, and network collectors to populate `SystemMetrics`
  directly instead of returning ad-hoc metric maps.
- Hardened process collector state tracking by adding the missing internal
  `cpu_percent` field used for top-process sorting.
- Updated `agent/agent.cpp` to use `std::make_unique` for collector ownership
  and safe `dynamic_cast` checks before changing process collector limits.

### Added
- BDD-style collector contract test covering polymorphic `name()` and
  `update(SystemMetrics&)` behavior.
- Collector contract documentation in `docs/collector-contract.md`.
- Bazel build notes in `docs/bazel-build.md`.

## [0.2.0] - 2026-04-29

### Added
- `server/server.hpp` / `server/server.cpp` — main `Server` class orchestrating
  the ZMQ ROUTER socket (metrics/commands data plane), enrollment REP socket,
  ZAP auth handler, REST API, and SQLite storage backend.
- `server/main.cpp` — server entry point with signal handling (`SIGINT`/`SIGTERM`),
  config loading, `--genkey` helper, and background thread management.
- `server/BUILD.bazel` — Bazel build rules; splits server into a `cc_library`
  (`server_lib`) and a `cc_binary` (`server`) so tests can link the library.
- `common_tests/protocol_test.cpp` — BDD-style tests for Frame round-trip
  encoding, EnrollRequest/Response payloads, CommandMessage, SystemMetrics
  nested serialization, and FrameType enum stability.
- `server_tests/store_test.cpp` — BDD-style tests covering all SQLite store
  operations: agent CRUD, upsert idempotency, approval, deletion, metrics
  insertion, limit queries, and `latest_metrics` per-agent aggregation.
- `server_tests/zap_handler_test.cpp` — BDD-style tests for ZAP key-set
  management: add, remove, duplicate add, harmless remove-of-absent key.
- Googletest `1.14.0` added to `MODULE.bazel` for test infrastructure.

### Fixed
- `server/api.cpp` — all four `CommandMessage::command_id` assignments were
  incorrectly casting `uint32_t` to `std::string`; replaced with
  `std::to_string(...)`.

## [0.1.0] - 2026-04-28

### Added
- Initial implementation by prior LLM pass:
  - `common/` — `metrics.hpp`, `protocol.hpp`, `commands.hpp`, `crypto.hpp`
  - `agent/` — `Agent` class with three `std::jthread` loops (IO, collection,
    heartbeat), six platform-aware metric collectors (CPU, memory, disk,
    network, process, temperature), config persistence, and enrollment client.
  - `server/` (partial) — REST API (`api.hpp/cpp`), SQLite store
    (`store_sqlite.hpp/cpp`), ZAP handler (`zap_handler.hpp/cpp`), server
    config (`config.hpp/cpp`), abstract `Store` interface (`store.hpp`).
  - Bazel build system: `MODULE.bazel`, `.bazelrc`, third-party BUILD stubs
    for libzmq, libsodium, cppzmq, msgpack-cxx, cpp-httplib, sqlite3.
