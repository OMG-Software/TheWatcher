# API authorization policy

Who may call what, per role. The table at the end is enforced by
`server_tests/api_authorization_matrix_test.cpp`: every route registered in
`server/api.cpp` is called once per role and must return the status stated
there. A new route needs a row in that test and a line here.

## Roles

| Role | Scope | Can change things |
|------|-------|-------------------|
| `global_admin` | every group | yes, plus users, groups, runbooks |
| `global_operator` | every group | yes (agents, enrollment, settings, views, silences, windows) |
| `global_viewer` | every group | no, read only |
| `group_admin` | own groups | agents, alerts, views, silences, windows of own groups; users of own groups |
| `group_operator` | own groups | as `group_admin` except user management |
| `group_viewer` | own groups | no, read only |

The legacy names `admin`, `operator` and `viewer` are stored as before and
behave exactly as `global_admin`, `global_operator` and `global_viewer`.

## Status codes

| Status | Meaning |
|--------|---------|
| 401 | no valid session |
| 403 | the role may never use the route, or the resource exists but is outside the caller's groups |
| 404 | the id does not exist (checked after the role, and for group users after group scope: an agent outside your groups is a 403, whether or not it exists) |
| 400 | malformed body, unknown group, or a value that is not allowed |
| 409 | the request conflicts with current state: an agent that is not pending, a duplicate key, or a stale configuration revision |

The API answers 403, not 404, for a resource that exists but is out of scope.
Agent, alert, silence, window and view ids are small integers or visible
names, so hiding existence would not hide much, and one rule is easier to
test. Do not change a single route to 404 without changing the rule.

## Rules worth knowing

- **Pending enrollments** belong to no group yet. Any global role may list
  them (a global viewer included); group roles may not. Approving or
  rejecting needs `global_operator` or `global_admin`.
- **Approval names its groups.** `POST /api/agents/:id/approve` needs a JSON
  body with `group_ids`; `[]` means "no group" and the agent is then visible to
  global roles only. There is no default group. The request is validated first
  (404 unknown agent, 400 bad or unknown group ids, 409 not pending or key
  already bound) and approval and groups are written in one transaction.
- **Agent groups** are changed by global operators and admins only. The
  Configure save (`collector_config`) accepts `group_ids` from a group user
  only when it equals the agent's current groups; a different list is a 403.
  Unknown group ids are a 400 everywhere.
- **Configure saves and concurrent edits.** `GET /api/agents` returns a
  `config_revision` for each agent, a hash of the fields the Configure modal
  saves. `collector_config` accepts it back as `config_revision`; if the agent
  has changed since, the save is refused with 409 and nothing is written.
  Leaving it out saves unconditionally.
- **Views.** Visibility is decided in one place in the API
  (`view_visible_to_session`):
  - global roles see every view;
  - a view with a group is visible to members of that group (`is_public` does
    not widen it);
  - a view with no group (group 0, including views created before groups) is
    visible to its owner, and to everyone when public.

  Operators edit and delete a view when they are global, own it, or it belongs
  to one of their groups. A view returned to a group user lists only the agents
  that user can access; an edit by a group user keeps the agents they cannot
  see.
- **Maintenance windows and silences.** Group users manage entries for agents in
  their groups. Windows for all agents (`*`) are listed to group users, read
  only; creating or deleting `*` windows or silences needs a global operator.
- **Settings, reports, groups and runbooks** are global. Creating groups,
  users with global roles, and runbooks needs `global_admin`.

## Role x route table

`-` means 401 (no session) for the first column. Columns: none, global
admin (GA), global operator (GO), global viewer (GV), group admin (GRA),
group operator (GRO), group viewer (GRV). A "scoped" row has two cases: a
resource in the caller's group (group users succeed per the role) and a
resource in another group (group users get 403).

| Route | none | GA | GO | GV | GRA | GRO | GRV |
|-------|------|----|----|----|-----|-----|-----|
| `POST /api/login`, `POST /api/logout` | public | | | | | | |
| `GET /api/session` | 401 | 200 | 200 | 200 | 200 | 200 | 200 |
| `GET /api/groups` | 401 | 200 | 200 | 200 | 200 | 200 | 200 |
| `POST /api/groups` | 401 | 200 | 403 | 403 | 403 | 403 | 403 |
| `GET /api/users` | 401 | 200 | 200 | 200 | 200 | 200 | 200 |
| `POST /api/users` | 401 | 200 | 403 | 403 | 200 (own groups, group roles only) | 403 | 403 |
| `PUT /api/users/:id/disable`, `/enable`, `DELETE /api/users/:id` | 401 | 200 | 403 | 403 | 403 | 403 | 403 |
| `PUT /api/users/:id/password` | 401 | any user | own | own | own | own | own |
| `GET /api/alerts`, `/unacknowledged` | 401 | 200 | 200 | 200 | 200 | 200 | 200 |
| `POST /api/alerts/:id/ack`, `DELETE /api/alerts/:id`, `bulk-ack`, `bulk-archive` | 401 | 200 | 200 | 403 | scoped | scoped | 403 |
| `GET /api/pending-enrollments` | 401 | 200 | 200 | 200 | 403 | 403 | 403 |
| `POST /api/agents/:id/approve`, `/reject` | 401 | 200 | 200 | 403 | 403 | 403 | 403 |
| `GET /api/agents`, `GET /api/metrics` | 401 | 200 | 200 | 200 | 200 | 200 | 200 |
| `POST /api/agents/:id/groups` | 401 | 200 | 200 | 403 | 403 | 403 | 403 |
| `DELETE /api/agents/:id` | 401 | 200 | 200 | 403 | scoped | scoped | 403 |
| `GET /api/metrics/:id`, `/log-matches`, `/history`, `GET /api/uptime/:id` | 401 | 200 | 200 | 200 | scoped | scoped | scoped |
| `POST /api/agents/:id/` `command`, `set_interval`, `set_process_limit`, `thresholds`, `collector_config`, `maintenance`, `pause`, `resume`, `restart_collectors`, `disconnect`, `get_status`, `description`, `runbook` | 401 | 200 | 200 | 403 | scoped | scoped | 403 |
| `GET /api/maintenance-windows`, `GET /api/silences` | 401 | 200 | 200 | 200 | 200 (own agents and, for windows, `*`) | same | same |
| `POST /api/maintenance-windows`, `POST /api/silences`, and `DELETE` of either | 401 | 200 | 200 | 403 | scoped (never `*`) | scoped (never `*`) | 403 |
| `GET /api/settings`, `PUT /api/settings`, `POST /api/reports/send` | 401 | 200 | 200 | 403 | 403 | 403 | 403 |
| `GET /api/runbooks` | 401 | 200 | 200 | 200 | 200 | 200 | 200 |
| `POST /api/runbooks`, `DELETE /api/runbooks/:id` | 401 | 200/201 | 403 | 403 | 403 | 403 | 403 |
| `GET /api/views`, `GET /api/views/:id` | 401 | 200 | 200 | 200 | visible views | visible views | visible views |
| `POST /api/views` | 401 | 201 | 201 | 403 | 201 (own group) | 201 (own group) | 403 |
| `PUT` / `DELETE /api/views/:id` | 401 | 200 | 200 | 403 | own or own-group views | own or own-group views | 403 |

The exact per-row statuses, including the other-group and no-group cases, are
in the test.

## Known gaps (not changed here)

- `DELETE /api/agents/:id` is open to group operators for agents of their
  groups. Deleting a host and its history is arguably an admin action.
- `PUT /api/users/:id/password` reports an unknown user (400) before checking
  permission (403), so a non-admin can tell whether a user id exists.
- `GET /api/settings` includes the webhook URLs and is open to global
  operators, not only admins.
- A non-numeric id in a path (`/api/alerts/abc/ack`, `/api/views/abc`)
  is answered 500 by some routes and 400 by others; the validation work
  (#184) makes it a uniform 400.
- `POST /api/agents/:id/reject` answers 200 for an unknown id and can reject an
  approved agent.
