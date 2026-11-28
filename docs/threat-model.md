# Threat Model

This document records the security model for TheWatcher as of 0.6.x. It is a
working document for audit, design review, and regression testing.

## Assets

- Server CURVE secret key and approved agent public keys.
- Agent CURVE secret keys and pinned server public-key fingerprints.
- Dashboard session cookies and user password hashes.
- Monitoring data, alert history, maintenance state, runbooks, and group
  assignments.
- Server database contents and local configuration files.
- Outbound webhook destinations for alerts and scheduled reports.

## Actors

- Global administrators and operators with fleet-wide privileges.
- Group administrators, operators, and viewers scoped to assigned groups.
- Approved agents with a server-approved CURVE public key.
- Pending agents on the plaintext enrollment endpoint.
- Unauthenticated network clients that can reach the API, enrollment socket, or
  data socket.
- A malicious browser page trying to drive an authenticated user's dashboard
  session.
- A compromised admin account attempting to use webhooks to reach internal
  services from the server host.

## Trust Boundaries

- Browser to REST API: cookie-authenticated HTTP. Production deployments should
  terminate TLS at a reverse proxy and bind the API to a trusted interface.
- Dashboard JavaScript to browser DOM: React escapes text by default; runbook
  markdown is stored as text and not rendered as HTML by the current dashboard.
- Agent enrollment to server: plaintext ZeroMQ REP used only to request
  approval and receive the server public key. First approval is trust-on-first-
  use; later approvals are checked against the pinned fingerprint. Agents fail
  closed: an approved reply must carry a key plus matching fingerprint, a
  reply contradicting a configured server key is rejected, and connecting
  without CURVE requires the explicit `--insecure` / `ALLOW_INSECURE=1`
  opt-in. An optional pre-shared `ENROLLMENT_TOKEN` on the server makes the
  enrollment endpoint unforgeable by arbitrary hosts. The reply is also
  untrusted text that is saved to the agent config: the server key must be
  exactly 40 Z85 characters decoding to a 32-byte key before it is used or
  stored, and a malformed or unreadable reply is dropped and retried on a new
  socket instead of stopping the agent.
- Agent data plane to server: ZeroMQ ROUTER with CURVE when the server has a
  public key configured. ZAP admits only approved agent public keys and
  reports the agent_id each key is bound to; every frame's claimed
  `agent_id` must match that authenticated identity.
- Server to database: local SQLite or PostgreSQL via configured DSN.
- Server to outbound webhooks: untrusted URLs stored by global operators/admins.

## Threats And Mitigations

### Spoofing

- Agent data spoofing is mitigated by CURVE plus ZAP approved-key checks, and
  the ZAP-authenticated identity is bound to the CURVE key: an approved agent
  cannot submit frames, ACK commands, or fetch config under another agent's
  id, and command ACKs are accepted only from the agent a command targeted.
- Enrollment spoofing is limited by manual approval, server-key pinning, an
  optional pre-shared enrollment token, strict field validation, and an
  immutable first-seen public key for pending records. First approval still
  relies on administrator judgment.
- Dashboard user spoofing is mitigated by libsodium password hashes and random
  256-bit session tokens. Only the SHA-256 of a token is stored, so a copy of
  the database or a backup holds no usable logins, once no writer of raw
  tokens remains: an old server during a rolling PostgreSQL upgrade still
  stores raw tokens until it is upgraded (they expire within eight hours).

### Tampering

- API mutations require authenticated roles and group checks.
- Group-scoped users cannot mutate global users, groups, settings, pending
  enrollment, or legacy global runbook links.
- SQL writes use parameter binding in SQLite and PostgreSQL stores.
- Config files are KEY=VALUE only and are bounded by size during parsing.
- All agent-supplied timestamps are advisory: metric rows, alert times and
  retention pruning run on the server's receive clock.
- Log-monitor paths and patterns are validated server-side, restricted to
  global admins, and enforced against an agent-local `LOG_ALLOWED_DIRS`
  allowlist the server cannot change.
- Store integrity: group membership saves are transactional, so a failed save
  cannot leave an agent or user with no groups (which would hide the agent
  from, or lock out, group-scoped users). A database read error is an
  exception, never "no rows": a locked database cannot make `is_silenced`
  return false or `get_session` log a user out silently. Heartbeats use
  narrow updates, so a stale snapshot cannot revert maintenance state or a
  collector config. Residual risk: a store call that fails mid-way surfaces
  as an HTTP 500 and the caller retries.

### Repudiation

- Alerts, silences, maintenance windows, runbooks, and user operations store the
  acting username where the current API contract supports it.
- Logs record security-relevant actions such as login failures, agent approval,
  command queueing, and webhook rejection.
- Authentication and authorization events (login success, failure and
  lockout, logout, every 403, user create/disable/enable/delete, password
  changes and session revocation) are written as `AUDIT` lines at NOTICE with
  the acting user, client address and target. Client-supplied values are
  sanitised so they cannot forge log lines. See `docs/logging.md`.

### Information Disclosure

- Session cookies are HttpOnly, SameSite=Strict, Secure, and scoped to `/`.
- Group-scoped API queries filter agents, agent history, alerts, metrics,
  runbooks (agent-scoped entries), views and view agent ids by shared group
  membership.
- PostgreSQL DSNs are logged with password values redacted.
- The generated server config is written atomically with owner-only
  permissions on non-Windows platforms, and the load path warns when an
  existing config is group/world readable. On Windows the installer grants
  only SYSTEM and Administrators on the config directory; the same is
  recommended for manually installed services.
- The agent config holds the CURVE secret key. It is written through an
  owner-only temporary file in the same directory that is flushed and renamed
  over the target (mode 0600 on POSIX; a protected DACL for SYSTEM,
  Administrators and the owner on Windows), every write error is reported, and
  a value containing CR or LF is refused so that no field, including the
  server key learned during enrollment, can inject extra config lines such as
  `SERVER_ADDRESS` or `ALLOW_INSECURE`.

### Denial Of Service

- Login failures are rate-limited per username (fixed window that resets on
  expiry) and throttled per source IP across all usernames.
- The client address used for throttling is taken from forwarding headers
  only when the TCP peer matches `TRUSTED_PROXIES` (addresses or CIDR ranges).
  Headers are parsed strictly; a malformed value goes to its own
  `unknown-client` bucket rather than the proxy's address, so a hostile client
  cannot throttle every user behind the proxy.
- The limit check and the failure count are one atomic step, so a burst of
  concurrent guesses cannot exceed the per-account limits while the password
  hash runs.
- A trusted range only matches peers of its own address family (`::/0` does not
  trust IPv4 peers, and IPv4-mapped IPv6 peers count as IPv4). A forwarding
  header that is present but empty, holds no address, or contains any empty or invalid entry (for example a trailing comma or junk left of the client) is treated as
  malformed (own bucket), never as "no header".
- A login attempt holds a slot in the throttle counters while its password is
  verified; the slot is returned on success or on an unexpected error, and
  only a genuine credential failure keeps it. A successful login therefore
  never erases the slots of other attempts still in flight. A slot is released
  only against the counter window it was taken in, so an attempt that outlives
  its window cannot decrement or erase a newer window's entry.
- A session is created only if the password hash the login verified is still
  the user's current hash and the user is still enabled (one atomic store
  statement), so a login that races a password reset or a disable cannot
  create a session after the old ones were revoked. The password update and
  the session revocation are one atomic store operation.
- Unknown and disabled usernames are verified against a dummy Argon2 hash
  (random password hashed at startup with the real parameters), so
  the response time of those paths is close to a real failure, which reduces
  (but does not eliminate) timing-based username enumeration.
- Sessions are revoked when a user's password changes (all of the user's
  sessions, except the caller's own on a self-service change), when a user is
  disabled (one atomic store operation, so re-enabling cannot restore old
  sessions; enabling also drops any leftover sessions; on PostgreSQL the login
  locks the user row with FOR SHARE and the change runs UPDATE then DELETE as
  two statements in one transaction) or deleted (the sessions
  table cascades). Enabling a user drops leftover sessions only on a real
  disabled-to-enabled change, in the same transaction; a repeated enable of an
  enabled user succeeds and revokes nothing. Expired sessions are purged at
  most every five minutes (a failed purge is retried) from login and session
  validation, and from the server's periodic job, so an idle server purges
  too. The five minutes are counted on a monotonic clock from the end of the
  last successful purge, so a slow purge or a wall-clock jump cannot make
  purges run back to back or stop them.
- A username containing a NUL byte is rejected (400) at login and user
  creation, before the throttle keys are built. The stores bind names as C
  strings, so otherwise `admin\0x` would reach `admin` under fresh throttle
  keys and sidestep the per-pair and per-username limits.
- A self-service password change is applied only while the stored hash is
  still the one the current password was verified against, the user is still
  enabled and the session that sent the request still exists and has not
  expired, all checked in the one UPDATE, in the same transaction that revokes
  sessions. An admin reset that lands in between wins: the change is refused
  with 409, nothing is revoked and no success is audited (`password_change`
  `conflict`). A disable (even one followed by an enable), a sign-out, an
  expiry or any other revocation of the caller's session that lands in between
  also wins: the change is refused with 401, nothing is changed and
  `password_change` `session_ended` is audited, so a session that was ended
  cannot still set a new password.
- The caller's session stays valid until the change commits. SQLite runs the
  change under `BEGIN IMMEDIATE`, the only write lock, so no sign-out can
  commit inside it. PostgreSQL locks the caller's session row `FOR SHARE`
  right after the UPDATE: a sign-out on another node that already deleted the
  row makes the change wait, and if that sign-out commits the change is
  refused (401) and rolled back; a sign-out that starts later waits for the
  change to commit. Both lock the users row before the sessions row, the
  order a disable and an admin reset use, and a sign-out locks only the
  sessions row, so none of them can deadlock.
- A wrong current password is audited once, as `password_change` `failure`.
  Handlers that audit their own 403 mark the response, and the post-routing
  hook then skips its generic `permission_denied` line; every other 403 is
  still audited there.
- Changing your own password (any role, including a global admin) needs the
  current password: a missing or non-string `current_password` is 400 and a
  wrong one is 403 with nothing changed and no session revoked. An admin reset
  of another user needs none. A wrong guess takes a slot in the same throttle
  counters as a failed login, under the same keys (client address plus the
  caller's username, the username alone, and the address alone), and is
  audited as `password_change` `failure`. Once a limit is reached the route
  answers 429 whatever the password. A stolen session therefore cannot
  brute-force the password any faster than a login can, and cannot lock the
  owner out by changing it.
- The `sessions` table stores `SHA-256(token)` as lowercase hex; both stores
  hash inside every method that takes a token, and `get_session` returns the
  caller's raw token, so a value read back from the store is never hashed
  twice. Tokens carry 256 bits of randomness, so an unsalted hash is enough.
  Upgrading from a version that stored raw tokens deletes all existing
  sessions once (everyone signs in again), marked by the `session_tokens_hashed`
  row in `server_settings` that is written in the same transaction as the
  delete, so the upgrade can neither hash a row twice nor run again.
- Every ZMQ socket caps inbound message size (`ZMQ_MAXMSGSIZE`: 64 KiB
  enrollment, 4 MiB data plane) and bounds the connection handshake
  (`ZMQ_HANDSHAKE_IVL`); enrollment replies use a send timeout so a stalled
  client cannot block the data-plane thread.
- CBOR payloads are structurally pre-validated (nesting depth cap, declared
  container counts must fit the remaining bytes) before the decoder can
  allocate from them; a malformed multipart enrollment request is drained and
  answered instead of wedging or crashing the REP state machine.
- Enrollment is rate-limited per agent_id and per source address with
  pruned tracking maps, and the pending-approval queue is size-capped.
- Approval and revocation are enforced per frame: rejected or deleted agents
  stop contributing data on their live session without waiting for a
  reconnect.
- Config parser file and line sizes are bounded to avoid unbounded local input.
- The API still needs a global request-body size limit in httplib or a fronting
  reverse proxy; this is a residual risk.
- An hourly retention job prunes metrics, status history, handled alerts, log
  matches, expired silences and ended maintenance windows, independent of
  agent traffic, so a flapping indicator or noisy log watch cannot grow the
  database without bound. Retention periods are configurable. Expired sessions
  are purged by the server's periodic job as well as from requests, so an idle
  server does not keep them.

### Elevation Of Privilege

- Role checks distinguish global and group-scoped privileges.
- Browser-origin checks reject credentialed cross-origin unsafe methods when an
  `Origin` or absolute `Referer` header is present. The comparison uses the
  browser origin tuple of scheme, host, and default-normalized port; local
  loopback dashboard ports are the only allowed different-port exception.
- Legacy global runbook mutations require `global_admin` because those runbooks
  are consumed by all alert generation.
- Log-monitor paths make an agent read local files and surface their contents
  on the dashboard, so configuring them requires `global_admin`; the agent
  additionally only reads regular files inside its local `LOG_ALLOWED_DIRS`.
- A log-monitor pattern that backtracks exponentially (`(a+)+$`, `(a|aa)*c`,
  `.*.*.*z`) would block the agent collection thread while it holds the
  collector lock (#169). `common/log_pattern.hpp` refuses back-references,
  repeated groups that contain a quantifier or alternation, back-to-back
  overlapping unbounded quantifiers and more than three unbounded
  quantifiers; the server rejects such a pattern with 400 when the config is
  saved and the agent refuses it again. Residual risk: the check is
  structural and conservative, so an unusual pattern that it accepts could
  still be slow on a 4 KiB line; a linear-time engine such as RE2 would remove
  that risk.
- The built-in first-run admin account cannot be disabled or deleted on either
  storage backend, and no-op disable/delete requests return an error instead
  of `{"ok":true}`.

## Webhook SSRF Controls

Alert and report webhook URLs are parsed by a shared validator. It accepts only
plain HTTP URLs supported by the current httplib build and rejects malformed
authorities, userinfo, invalid ports, localhost, loopback, private, link-local,
multicast, documentation, benchmark, and other non-public literal IP ranges.
IPv6 literals are parsed before range checks so alternate textual forms of the
same restricted address are handled consistently.

Webhook delivery runs on a separate worker thread with bounded queue (256), 2 s
or 5 s timeouts and limited retries. The URL is validated before a notification
is queued, so the worker only posts to validated targets. A slow or hostile
endpoint can no longer stall the data plane; at worst notifications are
dropped when the queue is full.

Hostname targets that resolve to private IPs after DNS lookup remain a residual
risk. Deployments should restrict outbound egress at the host or network layer
when webhooks are enabled.

## Residual Risks

- The API server is plain HTTP. Use a reverse proxy for TLS, HSTS, request body
  limits, and external exposure.
- Initial enrollment is trust-on-first-use. Operators must verify pending agent
  identity before approval; with `ENROLLMENT_TOKEN` unset, any reachable host
  can create pending records (bounded by rate limits and the queue cap).
- An attacker who can reach the login endpoint can repeatedly lock a single
  username out for 15 minutes at a time; per-source-IP throttling slows this
  but cannot prevent it from a distributed position.
- Behind a trusted proxy, `X-Real-IP` is believed as sent. A proxy that does
  not overwrite it lets clients choose their throttling bucket, so configure
  the proxy to set it (or strip it and rely on `X-Forwarded-For`).
- A self-service password change keeps the caller's current session. A stolen
  session cannot change the password without the current one, but it can
  still use the account until it expires. Signing out ends only the current
  session; other sessions of the account end when the password is changed
  (self-service keeps only the session that changed it), when an admin resets
  the password, or when an admin disables the account. There is no "sign out
  everywhere" action. Audit lines share the server log file and are not tamper
  proof.
- During a rolling upgrade on a shared PostgreSQL database, a server still on
  the old version stores raw tokens that upgraded servers do not recognise;
  those users sign in again, and the raw rows expire after eight hours.
- The built-in first-run `thewatcher` account has a documented default password.
  It must be changed or disabled immediately after bootstrap.
- Hostname webhook SSRF cannot be completely prevented without DNS resolution
  and connect-time IP validation.
- Agent compromise can still produce false local metrics for that host. The
  server authenticates the agent identity, not the truth of host telemetry.
- Log monitoring is disabled until the operator sets `LOG_ALLOWED_DIRS` on the
  agent; hardlinks to files outside the allowlist cannot be detected.
- Windows installs made before 0.8.0 keep the old config-directory ACL (Users:
  modify) until the 0.8.0 installer is re-run.
