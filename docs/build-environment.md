# Build Environment

This document describes the supported local build setup for TheWatcher.
Installation and runtime steps are covered in [Installation](installation.md)
and [Running](running.md).

TheWatcher is built with Meson + Ninja on every platform. ZeroMQ and libsodium
are built as local static subprojects from `subprojects/*.wrap` plus
`subprojects/packagefiles/`; system or vcpkg packages are not used.

## Requirements

### Windows

- Windows 10 or Windows 11, x64.
- PowerShell 5.1 or newer.
- `winget` from Microsoft App Installer.
- Git for Windows.
- Python 3.10+ (used to install Meson and to run a small header-copy step).
- Visual Studio 2022 Build Tools with the C++ workload and Windows SDK.
- CMake on PATH (Meson drives the libzmq subproject via CMake).
- Meson 1.4+ and Ninja, installed by the setup script via `pip install`.

```powershell
.\scripts\setup-build-env.cmd -SkipWsl
```

The setup script installs Git, Python 3.12, CMake, Visual Studio 2022 Build
Tools, and Node.js (LTS) via `winget` when they are missing, then installs
Meson and Ninja via `pip`. Run it from a normal PowerShell session unless you
also request WSL setup. The `.cmd` wrapper invokes PowerShell with a
process-scoped execution policy bypass; it does not change the machine or user
execution policy.

### Linux

The Linux setup script supports Debian/Ubuntu (apt), Fedora/RHEL/Rocky/Alma
(dnf), Arch/Manjaro (pacman), openSUSE/SLES (zypper), and Alpine (apk):

```bash
./scripts/setup-build-env-linux.sh
```

It installs the C++20 toolchain (gcc or clang), CMake, Ninja, pkg-config,
Python 3 + pip, Node.js, and then `pip install --user meson ninja` to
guarantee Meson 1.4+.

### BSD

The BSD setup script supports FreeBSD, DragonFly (pkg), OpenBSD (pkg_add), and
NetBSD (pkgin):

```sh
./scripts/setup-build-env-bsd.sh
```

It installs the C++20 toolchain, CMake, Ninja, pkg-config, Python 3 + pip,
Node.js, and `pip --user`-installs Meson and Ninja.

### Optional WSL

WSL is optional for the Windows build. Use it when you want to verify the
Linux build path from a Windows machine.

Run the setup script from an elevated PowerShell session:

```powershell
.\scripts\setup-build-env.cmd -InstallWsl -LinuxDistro Ubuntu
```

If WSL asks for a reboot or first-user setup, complete that step and rerun the
same command.

## Build Steps

Open a new PowerShell window after installing tools so `PATH` changes are
visible.

On Windows, run the Meson commands from a Visual Studio developer prompt so
`cl`, `link`, CMake, and Ninja are all on `PATH`. The repository also
provides a helper that always uses the release configuration:

```powershell
.\meson-build.cmd
```

Equivalent direct invocation, identical on every platform:

```bash
meson setup builddir-release --buildtype=release --default-library=static
meson compile -C builddir-release
meson test    -C builddir-release --print-errorlogs
```

The root Meson project builds TheWatcher code with `warning_level=3` and
`werror=true`. Meson fallback dependencies are configured separately with
`warning_level=0` and `werror=false` so third-party warnings do not fail local
or CI builds.

Run the full server-agent integration path directly with:

```bash
meson test -C builddir-release server_agent_integration_test --print-errorlogs
```

That test starts the server, enrolls and approves an agent, verifies collected
metrics reach the server, sends a disconnect command, and shuts both sides
down.

## CI Build Artifacts

Each CI platform job archives its `builddir-release/` directory as a GitHub
Actions artifact, even when the build or tests fail. The Linux, Windows, and
FreeBSD jobs also publish a download link in the job summary so the archived
build output can be opened directly from the workflow run page.

Build the dashboard separately with npm. The dashboard needs Node.js
`^22.12.0 || ^24.0.0 || >=26.0.0` (declared as `engines` in
`dashboard/package.json`; Vite 8 and Vitest 5 do not support older releases).
`dashboard/.npmrc` sets `engine-strict`, so `npm install` stops with an error on
an unsupported release. Distribution packages are often older than this (for
example Ubuntu 24.04 ships Node.js 18); the Linux and BSD setup scripts warn
when the installed Node.js is outside the range. Install a supported release
from <https://nodejs.org/en/download> (or `pkg install node24 npm-node24` on
FreeBSD).

```powershell
cd dashboard
npm.cmd install
npm.cmd run build
npm.cmd test
```

## Subprojects and the package cache

The first `meson setup` downloads dependency source archives into
`subprojects/packagecache` and extracts build-local source directories under
`subprojects/`. Those extracted directories are generated files and are
ignored by Git; the committed source of truth is the wrap files plus the
packagefile overlays:

- `libsodium` from `subprojects/libsodium.wrap` and
  `subprojects/packagefiles/libsodium`.
- `libzmq` from `subprojects/libzmq.wrap` and
  `subprojects/packagefiles/libzmq`, with CURVE enabled and linked to the
  local static libsodium build.
- `cppzmq`, `libcbor`, `sqlite3`, `nlohmann_json`, `cpp-httplib`, and
  `catch2` from WrapDB.

## Troubleshooting

### `meson: command not found`

Open a new shell after running the setup script. On Linux/BSD, ensure
`$HOME/.local/bin` is on `PATH` (where pip installs `meson` and `ninja` with
the `--user` flag).

### Visual C++ compiler not detected (Windows)

Rerun `setup-build-env.cmd`. If Visual Studio Build Tools was just installed,
reboot or open a new Visual Studio developer PowerShell before building.

### CMake cannot find a generator

Meson uses CMake only to drive the libzmq subproject build. On Windows, make
sure you are in a developer prompt so `cl` and `link` are visible; on
Linux/BSD, ensure `ninja` is on `PATH`.
