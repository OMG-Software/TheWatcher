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
| TheWatcher | ≥ 0.5.0 | compiled with `HAVE_LIBPQ` |
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

## TheWatcher configuration (`TheWatcherServer.conf`)

```text
DB_TYPE=postgres
POSTGRES_DSN=host=10.0.0.20 port=6432 dbname=thewatcher user=thewatcher password=change-me sslmode=require connect_timeout=5

BIND_ADDRESS=tcp://10.0.0.10:5555
ENROLLMENT_ADDRESS=tcp://10.0.0.10:5556
API_HOST=0.0.0.0
API_PORT=8080
```

`BIND_ADDRESS` and `ENROLLMENT_ADDRESS` use the VIP. See
[Configuration](configuration.md) for every option. The store uses the schema
that `search_path` selects (`public` unless the DSN says
`options=-csearch_path=myschema`); the server creates and migrates its tables
on start, and nodes that start together wait for each other, so both nodes
can use the same config and start at once.

Use the same config on both nodes.  Only the active node will hold the VIP.

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
6. TheWatcherServer reconnects to PostgreSQL through pgBouncer (the store
   resets a dead connection by itself; no process restart) and resumes
   normal operation. Committed state is in PostgreSQL, so failover does not
   lose committed data. A write that was in flight when the connection died
   is not retried, because it may already have been applied, so it can fail:
   the server logs it and carries on (the periodic scan catches the error
   instead of ending the server loop). A dropped metrics sample is
   simply missing (the agent sends the next one); the periodic scans
   (offline detection, dead-agent and heartbeat alerts, escalation) and
   agent heartbeats re-derive their writes from stored state and repeat on
   the next tick. Reads are retried once on the new connection.

Typical failover time is **5–15 seconds** depending on `advert_int` and
agent reconnect intervals.

---

## Caveats

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
