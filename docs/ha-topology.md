# High-Availability Topology

TheWatcher supports a two-server active/standby topology backed by a shared
PostgreSQL database.  This document describes a reference setup using
**pgBouncer** for connection pooling and **keepalived** for virtual-IP
failover.

---

## Architecture overview

```
Agents
  │  (ZeroMQ CURVE over TCP)
  ▼
┌────────────────────┐    ┌────────────────────┐
│  TheWatcherServer  │    │  TheWatcherServer  │
│  (primary)         │    │  (standby)         │
│  VIP: 10.0.0.10   │    │  (standby only)    │
└────────┬───────────┘    └──────────┬─────────┘
         │                           │
         └──────────┬────────────────┘
                    │ (connection pool)
              ┌─────▼──────┐
              │  pgBouncer  │
              │  :6432      │
              └─────┬───────┘
                    │
         ┌──────────▼──────────┐
         │  PostgreSQL primary │
         │  :5432              │
         └─────────────────────┘
```

At any time only **one** TheWatcherServer instance is active.  keepalived
holds the virtual IP (VIP) on whichever node is primary.  Agents always
connect to the VIP so failover is transparent to them.

---

## Prerequisites

| Component | Version | Notes |
|---|---|---|
| TheWatcher | current release | built with PostgreSQL support, see [Build Environment](build-environment.md) |
| PostgreSQL | ≥ 14 | streaming replication optional but recommended |
| pgBouncer | ≥ 1.18 | transaction-pool mode |
| keepalived | ≥ 2.2 | VRRP for VIP management |

---

## PostgreSQL setup

1. Create the database and a dedicated user:

   ```sql
   CREATE USER thewatcher WITH PASSWORD 'change-me';
   CREATE DATABASE thewatcher OWNER thewatcher;
   ```

2. Grant the user access in `pg_hba.conf`:

   ```
   host  thewatcher  thewatcher  10.0.0.0/24  scram-sha-256
   ```

3. TheWatcher will create all tables on first connection (`init_schema`).

---

## pgBouncer setup (`/etc/pgbouncer/pgbouncer.ini`)

```ini
[databases]
thewatcher = host=10.0.0.20 port=5432 dbname=thewatcher

[pgbouncer]
listen_addr = 0.0.0.0
listen_port = 6432
auth_type   = scram-sha-256
auth_file   = /etc/pgbouncer/userlist.txt
pool_mode   = transaction
max_client_conn = 200
default_pool_size = 20
```

`/etc/pgbouncer/userlist.txt`:
```
"thewatcher" "change-me"
```

Both TheWatcherServer nodes connect to pgBouncer on the same host or on a
dedicated pgBouncer host.

---

## TheWatcher configuration (`/etc/thewatcher/TheWatcherServer.conf`)

The server reads a `KEY=VALUE` file with upper-case keys, one per line (see
[Configuration](configuration.md)). Other formats such as TOML or JSON are not
read; the server would ignore them and start with SQLite.

```text
DB_TYPE=postgres
POSTGRES_DSN=host=10.0.0.20 port=6432 dbname=thewatcher user=thewatcher password=change-me sslmode=require

BIND_ADDRESS=tcp://10.0.0.10:5555
ENROLLMENT_ADDRESS=tcp://10.0.0.10:5556
API_HOST=0.0.0.0
API_PORT=8080
SERVER_PUBLIC_KEY=<same 40-character z85 key on both nodes>
SERVER_SECRET_KEY=<same 40-character z85 key on both nodes>
```

`BIND_ADDRESS` and `ENROLLMENT_ADDRESS` use the VIP, so the standby can only
bind them once it holds the VIP. Both nodes must use the same
`SERVER_PUBLIC_KEY` / `SERVER_SECRET_KEY` pair: agents pin the server public
key, so a node with a different key is rejected after failover. Keep the file
owner-only, because it holds the CURVE secret and the database password.

A server built without PostgreSQL support exits at startup with
`Unsupported db_type: postgres`, so a wrong binary is caught immediately.

---

## keepalived setup

### Primary node (`/etc/keepalived/keepalived.conf`)

```
vrrp_script chk_thewatcher {
    script "systemctl is-active --quiet TheWatcherServer"
    interval 2
    weight   -20
}

vrrp_instance VI_1 {
    state            MASTER
    interface        eth0
    virtual_router_id 51
    priority         100
    advert_int       1

    authentication {
        auth_type PASS
        auth_pass change-me
    }

    virtual_ipaddress {
        10.0.0.10/24
    }

    track_script {
        chk_thewatcher
    }

    notify_master "/usr/local/bin/thewatcher-promote.sh"
    notify_backup "/usr/local/bin/thewatcher-demote.sh"
}
```

### Standby node

Same configuration with `state BACKUP` and `priority 90`.

### Promote / demote scripts

**`/usr/local/bin/thewatcher-promote.sh`**
```bash
#!/bin/bash
systemctl start TheWatcherServer
```

**`/usr/local/bin/thewatcher-demote.sh`**
```bash
#!/bin/bash
systemctl stop TheWatcherServer
```

Make both scripts executable: `chmod +x /usr/local/bin/thewatcher-*.sh`

---

## Failover behaviour

1. keepalived detects that the primary's health check fails (or the node
   goes down entirely).
2. VRRP elects the standby node as the new master.
3. The VIP is moved to the standby node.
4. The `notify_master` script starts TheWatcherServer on the standby.
5. Agents reconnect automatically — their ZeroMQ sockets will retry until
   the new endpoint accepts connections.
6. TheWatcherServer connects to PostgreSQL through pgBouncer and resumes
   normal operation.  Persistent state (agents, metrics, users, sessions, alerts and settings) is in
   PostgreSQL and survives the failover. State held in memory by the failed
   node is lost: queued commands that were not yet delivered to an agent, and
   login throttling counters.

Typical failover time is **5–15 seconds** depending on `advert_int` and
agent reconnect intervals.

---

## Caveats

- **No single-writer guard**: the server does not check whether another
  node is already running against the same database. Two active nodes both
  write to PostgreSQL, so keep the standby stopped until it takes the VIP
  (the `notify_master` / demote scripts above do this).
- **Split-brain**: Both nodes could briefly believe they are primary.
  To guard against duplicate alerts, ensure the ZeroMQ enrollment port
  is only reachable via the VIP (firewall the per-node IPs).
- **pgBouncer single point of failure**: Run pgBouncer on both nodes or use
  a dedicated pgBouncer pair behind its own VIP if needed.
- **PostgreSQL HA**: This document does not cover PostgreSQL replication.
  For full HA, add a PostgreSQL streaming replica and use Patroni or
  pg_auto_failover to manage promotion.
- **TLS**: Always set `sslmode=require` (or higher) in the DSN when
  PostgreSQL is on a separate host.
