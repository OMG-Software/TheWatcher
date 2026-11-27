# Build Environment

This document describes the supported local build setup for TheWatcher.
Installation and runtime steps are covered in [Installation](installation.md)
and [Running](running.md).

TheWatcher is built with Meson + Ninja on every platform. ZeroMQ and libsodium
are built as local static subprojects from `subprojects/*.wrap` plus
`subprojects/packagefiles/`; system or vcpkg packages are not used by default.
The one opt-in exception is `-Dsystem_zeromq=true` (see
[Build options](#build-options)).

## Requirements

### Windows

- Windows 10 or Windows 11, x64.
- PowerShell 5.1 or newer.
- `winget` from Microsoft App Installer.
- Git for Windows.
- Python 3.10+ (used to install Meson and to run a small header-copy step).
- Visual Studio 2022 or newer (Build Tools are enough) with the C++ x64 tools
  and Windows SDK. `meson-build.cmd` finds it with `vswhere.exe`, so the
  Visual Studio major version and install path do not matter.
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
Python 3 + pip, the libpq development package (PostgreSQL support), Node.js,
and then Meson and Ninja (Meson 1.4+) through `pipx` when available, falling
back to a managed virtual environment and finally to
`pip --break-system-packages`. `pip install --user` is not used directly
because current distributions mark the system Python as externally managed.

### BSD

The BSD setup script supports FreeBSD, DragonFly (pkg), OpenBSD (pkg_add), and
NetBSD (pkgin):

```sh
./scripts/setup-build-env-bsd.sh
```

It installs the C++20 toolchain, CMake, Ninja, pkg-config, Python 3, the libpq
client library (newest `postgresql*-client` the repository offers), Node.js and
Meson. Package names are unversioned wherever the BSD offers an unversioned
name, so the script does not break when a repository drops an old version.
Pass `--skip-postgres` to omit libpq.

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
provides a helper that always uses the release configuration. It uses the
compiler already on `PATH` when there is one, otherwise it locates Visual
Studio with `vswhere.exe` and loads `vcvars64.bat`. It stops with a non-zero
exit code on the first failing step:

```powershell
.\meson-build.cmd
```

Equivalent direct invocation, identical on every platform:

```bash
meson setup builddir-release --buildtype=release --default-library=static
meson compile -C builddir-release
meson test    -C builddir-release --suite thewatcher --print-errorlogs
```

`--suite thewatcher` selects TheWatcher's own tests. Dependency subprojects
(Catch2, cppzmq) have their test suites disabled in `meson.build`, so a bare
`meson test` runs the same set; the wrapper scripts and CI pass the suite
explicitly so a future subproject cannot slip in. List the test names with
`meson test -C builddir-release --list`; they differ from the executable
target names (`server_config`, not `server_config_test`).

The root Meson project builds TheWatcher code with `warning_level=3` and
`werror=true`. Meson fallback dependencies are configured separately with
`warning_level=0` and `werror=false` so third-party warnings do not fail local
or CI builds.

## Build options

| Option | Default | Meaning |
|---|---|---|
| `-Dpostgres=auto`, `enabled` or `disabled` | `auto` | PostgreSQL storage backend. `auto` builds it when libpq is found, `enabled` fails `meson setup` when libpq is missing, `disabled` omits it. |
| `-Dsystem_zeromq=true` | `false` | Use pkg-config `libzmq` (>= 4.3.2) and `libsodium` (>= 1.0.18) instead of the local static subprojects. Fails when they are missing. `cppzmq` always comes from its wrap. For distribution packaging only; CI covers the default. |

PostgreSQL needs the libpq client library and headers at build time:

| Platform | Package |
|---|---|
| Debian / Ubuntu | `libpq-dev` |
| Fedora / RHEL | `libpq-devel` |
| Arch | `postgresql-libs` |
| openSUSE | `postgresql-devel` |
| Alpine | `libpq-dev` |
| FreeBSD | `postgresql<N>-client` (newest available) |
| OpenBSD | `postgresql-client` |
| NetBSD | `postgresql<N>-client` |
| Windows | not built by default; install PostgreSQL's client SDK and make `libpq.pc` visible to pkg-config to enable it |

The setup scripts install libpq. Without it a default (`auto`) build silently
omits PostgreSQL and `DB_TYPE=postgres` fails at startup, so release builds
should pass `-Dpostgres=enabled`. `TheWatcherServer --version` prints
`PostgreSQL support: enabled` or `disabled` for the binary you have.

## Versioning

`meson.build` (`project(... version: ...)`) is the single source of truth. It
is passed to every C++ target as `THEWATCHER_VERSION` (see `common/version.hpp`)
and `TheWatcherServer --version` / `TheWatcherAgent --version` print it.
`dashboard/package.json`, `dashboard/package-lock.json`,
`packaging/windows/TheWatcher.iss` and the newest `CHANGELOG.md` section must
carry the same number. `scripts/check_version_consistency.py` verifies this; it
runs as the `version_consistency` Meson test and in a CI job. Bump all of them
together.

## Sanitizer and debug builds

CI has a Linux job that builds with AddressSanitizer and
UndefinedBehaviorSanitizer in a debug configuration (assertions enabled) and
runs the whole suite. Run it locally with:

```bash
meson setup builddir-asan --buildtype=debug -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C builddir-asan
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  meson test -C builddir-asan --suite thewatcher --print-errorlogs
```

Run the full server-agent integration path directly with:

```bash
meson test -C builddir-release server_agent_integration --print-errorlogs
```

That test starts the server, enrolls and approves an agent, verifies collected
metrics reach the server, sends a disconnect command, and shuts both sides
down.

## CI Build Artifacts

CI runs these jobs: `clang-format`, `version-consistency`, `Linux`, `Linux
(ASan + UBSan, debug)`, `Windows`, and `FreeBSD`. The platform and sanitizer
jobs run `meson compile` (which builds every target, including
`TheWatcherServer` and `TheWatcherAgent`) and then the `thewatcher` test suite.

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
- `libcbor` from its upstream GitHub release archive
  (`subprojects/libcbor.wrap`), built with the local Meson overlay in
  `subprojects/packagefiles/libcbor` (`patch_directory`).
- `cppzmq`, `sqlite3`, `nlohmann_json`, `cpp-httplib`, and `catch2` from
  WrapDB.

## Troubleshooting

### `meson: command not found`

Open a new shell after running the setup script. On Linux/BSD, ensure
`$HOME/.local/bin` is on `PATH` (where pip installs `meson` and `ninja` with
the `--user` flag).

### Visual C++ compiler not detected (Windows)

Rerun `setup-build-env.cmd`. `meson-build.cmd` reports "No Visual Studio 2022+
installation with the C++ x64 tools was found" when `vswhere.exe` sees no
install with the `VC.Tools.x86.x64` component. If Visual Studio Build Tools was
just installed, reboot or open a new Visual Studio developer PowerShell before
building.

### CMake cannot find a generator

Meson uses CMake only to drive the libzmq subproject build. On Windows, make
sure you are in a developer prompt so `cl` and `link` are visible; on
Linux/BSD, ensure `ninja` is on `PATH`.
