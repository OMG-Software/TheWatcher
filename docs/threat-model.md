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
  enrollment endpoint unforgeable by arbitrary hosts.
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
  256-bit session tokens.

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

### Repudiation

- Alerts, silences, maintenance windows, runbooks, and user operations store the
  acting username where the current API contract supports it.
- Logs record security-relevant actions such as login failures, agent approval,
  command queueing, and webhook rejection.

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

### Denial Of Service

- Login failures are rate-limited per username (fixed window that resets on
  expiry) and throttled per source IP across all usernames.
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
- Metrics and alert retention settings should be configured to limit database
  growth in production.

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
