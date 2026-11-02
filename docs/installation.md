# Installation

This document covers installing TheWatcher after the build environment is
ready. Build tool setup is covered in [Build Environment](build-environment.md).

## Prerequisites

### Server

- Windows 10/11 x64, Linux, or BSD.
- A writable config/log directory.
- Network listeners available for:
  - `5555/tcp` agent data and commands;
  - `5556/tcp` enrollment;
  - `8080/tcp` REST API.
- SQLite support from the built binary. No external SQLite service is required.
- The generated server CURVE public key is returned to agents in approved
  enrollment responses and then pinned by the agent.

### Agent

- Windows 10/11 x64, Linux, or BSD.
- Outbound network access to the server on `5555/tcp` and `5556/tcp`.
- A writable config/log directory.
- `THEWATCHER_SERVER=<server-host-or-ip>` in `TheWatcherAgent.conf`.
- Optional `SERVER_PUBLIC_KEY=<server-public-key>` and
  `SERVER_PUBLIC_KEY_FINGERPRINT=<fingerprint>` only when pre-pinning a known
  server identity before first enrollment.
- User-space permission to read the local OS counters used by the collectors.

### Dashboard

- Node.js and npm for building or running the Vite dev server.
- Server REST API reachable from the browser or from the web server proxy.
- For development: Vite dev server on `127.0.0.1:5173`.
- For public use: nginx, Apache httpd, or another static web server serving
  `dashboard/dist` and proxying `/api` to the TheWatcher server API.

The Vite dev server is intended for development. For public access, build the
dashboard and serve the static files with a web server.

### Build Machine

- Meson 1.4+, Ninja, and the C++ toolchain described in
  [Build Environment](build-environment.md).
- CMake on PATH (Meson drives the libzmq subproject build via CMake).
- Visual Studio 2022 Build Tools on Windows.
- npm for dashboard build and tests.

## Build Artifacts

Build the server and agent from the repository root:

```powershell
.\meson-build.cmd
```

Equivalent direct invocation:

```bash
meson setup builddir-release --buildtype=release --default-library=static
meson compile -C builddir-release
```

The Windows binaries are written under:

```text
builddir-release\server\TheWatcherServer.exe
builddir-release\agent\TheWatcherAgent.exe
```

The dashboard production bundle is written to `dashboard\dist`:

```powershell
cd dashboard
npm.cmd install
npm.cmd run build
```

The C++ server currently exposes the REST API. It does not serve the dashboard
production bundle itself, so run the Vite dev server during development or host
`dashboard\dist` with a separate static web server.

Default dashboard login after first database initialization:

```text
username: thewatcher
password: look_at_me
```

Change this account immediately after initial setup once user-editing controls
are available for your deployment process.

## Linux And BSD Dashboard Publishing

Build the dashboard on the host that will publish it:

```bash
cd dashboard
npm install
npm run build
```

Copy or keep the generated `dashboard/dist` directory somewhere readable by the
web server. The examples below use:

```text
/opt/thewatcher/dashboard/dist
```

The examples assume the TheWatcher server API listens on `127.0.0.1:8080`.

### nginx Static Dashboard

Recommended public deployment: nginx serves the static dashboard and proxies
only `/api` to the TheWatcher server.

```nginx
server {
    listen 80;
    server_name watcher.example.com;

    root /opt/thewatcher/dashboard/dist;
    index index.html;

    location /api/ {
        proxy_pass http://127.0.0.1:8080/api/;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }

    location / {
        try_files $uri $uri/ /index.html;
    }
}
```

### Apache httpd Static Dashboard

Enable the required proxy modules if they are not already enabled. Module names
vary by distribution, but Apache needs `mod_proxy`, `mod_proxy_http`,
`mod_headers`, and normal static file serving.

```apache
<VirtualHost *:80>
    ServerName watcher.example.com
    DocumentRoot "/opt/thewatcher/dashboard/dist"

    <Directory "/opt/thewatcher/dashboard/dist">
        Require all granted
        Options -Indexes
        AllowOverride None
    </Directory>

    ProxyPreserveHost On
    ProxyPass        "/api/" "http://127.0.0.1:8080/api/"
    ProxyPassReverse "/api/" "http://127.0.0.1:8080/api/"

    RewriteEngine On
    RewriteCond "%{REQUEST_FILENAME}" !-f
    RewriteCond "%{REQUEST_FILENAME}" !-d
    RewriteRule "^/(.*)$" "/index.html" [L]
</VirtualHost>
```

### Development-Only nginx Proxy To Vite

Use this only when actively developing the dashboard. Keep Vite bound to
loopback and let nginx expose it.

```bash
cd dashboard
npm run dev -- --host 127.0.0.1
```

```nginx
server {
    listen 80;
    server_name watcher-dev.example.com;

    location /api/ {
        proxy_pass http://127.0.0.1:8080/api/;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }

    location / {
        proxy_pass http://127.0.0.1:5173/;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
    }
}
```

### Development-Only Apache httpd Proxy To Vite

Use this only when actively developing the dashboard.

```apache
<VirtualHost *:80>
    ServerName watcher-dev.example.com

    ProxyPreserveHost On
    ProxyPass        "/api/" "http://127.0.0.1:8080/api/"
    ProxyPassReverse "/api/" "http://127.0.0.1:8080/api/"

    ProxyPass        "/" "http://127.0.0.1:5173/"
    ProxyPassReverse "/" "http://127.0.0.1:5173/"
</VirtualHost>
```

## Recommended Windows Layout

Use `C:\ProgramData\TheWatcher` for service configs and logs:

```powershell
New-Item -ItemType Directory -Force C:\ProgramData\TheWatcher
```

Recommended files:

```text
C:\ProgramData\TheWatcher\server.json
C:\ProgramData\TheWatcher\TheWatcherServer.log
C:\ProgramData\TheWatcher\TheWatcherAgent.conf
C:\ProgramData\TheWatcher\TheWatcherAgent.log
```

The server creates `server.json` on first run if it does not exist. The agent
creates `TheWatcherAgent.conf` on first run if it does not exist.

The config directory holds the server and agent CURVE secret keys. On POSIX
installs the generated config files are owner-only (`0600`). On Windows,
restrict the directory to SYSTEM and Administrators (the 0.8.0 installer
does this automatically; for manual layouts run the equivalent of):

```powershell
icacls C:\ProgramData\TheWatcher /inheritance:r /grant:r "*S-1-5-32-544:(OI)(CI)F" "*S-1-5-18:(OI)(CI)F"
```

## Foreground Install

Foreground mode is the simplest local setup. No service registration is needed.

Create the server config and print its public key:

```powershell
.\builddir-release\server\TheWatcherServer.exe --config C:\ProgramData\TheWatcher\server.json
```

Stop it with `Ctrl+C` if you only wanted to create the config. Normal agents
learn the public key from the approved enrollment response, so copying the key
into each agent config is not required.

Create or edit `C:\ProgramData\TheWatcher\TheWatcherAgent.conf`:

```text
THEWATCHER_SERVER=127.0.0.1
```

Start the agent:

```powershell
.\builddir-release\agent\TheWatcherAgent.exe --config C:\ProgramData\TheWatcher\TheWatcherAgent.conf
```

Open the dashboard and approve the pending agent.

## Windows Services

Install the server service:

```powershell
.\builddir-release\server\TheWatcherServer.exe --install-service --config C:\ProgramData\TheWatcher\server.json
```

Install the agent service:

```powershell
.\builddir-release\agent\TheWatcherAgent.exe --install-service --config C:\ProgramData\TheWatcher\TheWatcherAgent.conf
```

Start and stop services:

```powershell
sc.exe start TheWatcherServer
sc.exe stop TheWatcherServer
sc.exe start TheWatcherAgent
sc.exe stop TheWatcherAgent
```

Uninstall services:

```powershell
.\builddir-release\server\TheWatcherServer.exe --uninstall-service
.\builddir-release\agent\TheWatcherAgent.exe --uninstall-service
```

Use `--service-name <name>` on install, service mode, and uninstall when
running multiple instances on one machine.

## Windows EXE Installer

The repository includes an Inno Setup script:

```text
packaging\windows\TheWatcher.iss
```

Prerequisites:

- Build the server and agent with `.\meson-build.cmd` (or `meson compile -C
  builddir-release TheWatcherServer TheWatcherAgent`).
- Build the dashboard with `npm.cmd run build` if the installer should include
  static dashboard files.
- Install Inno Setup 6 on the packaging machine.

Build the installer:

```powershell
iscc.exe packaging\windows\TheWatcher.iss
```

The installer writes application files under:

```text
C:\Program Files\TheWatcher
```

It creates `C:\ProgramData\TheWatcher`, installs `TheWatcherServer` and
`TheWatcherAgent` as Windows services using their existing service install
flags, and removes both services on uninstall. Since 0.8.0 the installer
resets the config directory ACL to SYSTEM and Administrators only — earlier
installers granted `Users:modify`, so upgrades should re-run the 0.8.0
installer to pick up the restricted ACL. Edit
`C:\ProgramData\TheWatcher\TheWatcherAgent.conf` after installation so
`THEWATCHER_SERVER` points at the correct server before starting the agent
service. The agent writes `SERVER_PUBLIC_KEY` and
`SERVER_PUBLIC_KEY_FINGERPRINT` after the first approved enrollment.

## Linux And BSD

Linux and BSD builds use the same Meson targets. Run the binaries in
foreground mode under a host supervisor such as systemd, rc.d, or another
service manager. The Windows service flags intentionally return an
explanatory error outside Windows.

Typical config paths:

```text
/etc/thewatcher/server.json
~/.config/thewatcher/TheWatcherAgent.conf
```

For daemon deployments, prefer explicit config paths owned by the service user
and pass `--config <path>` in the service definition.

## Network Ports

Defaults:

- `5555/tcp`: ZeroMQ agent data, heartbeats, command ACKs, and config refresh.
- `5556/tcp`: ZeroMQ enrollment.
- `8080/tcp`: HTTP REST API.
- `5173/tcp`: Vite dashboard dev server.

Agents connect outbound to the server. The server does not initiate network
connections to agents.
