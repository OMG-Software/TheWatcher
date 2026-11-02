#!/bin/sh
# Set up the TheWatcher build environment on BSD systems.
#
# Installs the Meson + Ninja toolchain and the C++20 compiler stack. Detects
# the host BSD from `uname -s` and uses the matching package manager.
#
# Supported systems:
#   - FreeBSD   (pkg)
#   - OpenBSD   (pkg_add)
#   - NetBSD    (pkgin)
#   - DragonFly (pkg)
#
# Usage:
#   ./scripts/setup-build-env-bsd.sh [options]
#
# Options:
#   --skip-packages   Do not install packages.
#   --skip-meson      Do not pip-install Meson/Ninja.
#   --skip-node       Do not install Node.js/npm (dashboard build).
#   --no-verify       Skip the post-install verification step.
#   -h | --help       Show this help and exit.

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SKIP_PACKAGES=0
SKIP_MESON=0
SKIP_NODE=0
NO_VERIFY=0

usage() {
    sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

for arg in "$@"; do
    case "$arg" in
        --skip-packages) SKIP_PACKAGES=1 ;;
        --skip-meson)    SKIP_MESON=1 ;;
        --skip-node)     SKIP_NODE=1 ;;
        --no-verify)     NO_VERIFY=1 ;;
        -h|--help)       usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

log_step() { printf '\n==> %s\n' "$*"; }
log_info() { printf '    %s\n' "$*"; }
log_warn() { printf '    WARNING: %s\n' "$*" >&2; }
die()      { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

has_cmd() { command -v "$1" >/dev/null 2>&1; }

# Node.js range the dashboard toolchain supports. Keep this in step with
# "engines" in dashboard/package.json: Vite 8 needs ^20.19.0 || >=22.12.0 and
# Vitest 5 needs ^22.12.0 || ^24.0.0 || >=26.0.0; this is their intersection.
DASHBOARD_NODE_RANGE='^22.12.0 || ^24.0.0 || >=26.0.0'

# Succeeds when the node on PATH satisfies DASHBOARD_NODE_RANGE.
node_version_supported() {
    node_v=$(node --version 2>/dev/null) || return 1
    node_v=${node_v#v}
    node_major=${node_v%%.*}
    node_rest=${node_v#*.}
    node_minor=${node_rest%%.*}
    case "$node_major" in '' | *[!0-9]*) return 1 ;; esac
    case "$node_minor" in '' | *[!0-9]*) return 1 ;; esac
    if [ "$node_major" -eq 22 ] && [ "$node_minor" -ge 12 ]; then return 0; fi
    if [ "$node_major" -eq 24 ] || [ "$node_major" -ge 26 ]; then return 0; fi
    return 1
}

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
    if has_cmd doas; then
        SUDO="doas"
    elif has_cmd sudo; then
        SUDO="sudo"
    else
        die "This script needs root privileges, doas, or sudo to install packages."
    fi
fi

detect_bsd() {
    BSD_KIND="$(uname -s)"
    case "$BSD_KIND" in
        FreeBSD|DragonFly|OpenBSD|NetBSD) ;;
        *) die "Unsupported BSD: $BSD_KIND. Use the Linux or Windows script instead." ;;
    esac
    log_info "Detected BSD: ${BSD_KIND}"
}

# FreeBSD and DragonFly both ship pkg(8).
install_packages_freebsd() {
    $SUDO pkg install -y \
        bash \
        gcc \
        llvm \
        git \
        pkgconf \
        autoconf automake libtool gmake cmake ninja \
        unzip zip curl ca_root_nss \
        python3 py39-pip \
        gawk
    if [ "$SKIP_NODE" -eq 0 ]; then
        $SUDO pkg install -y node npm
    fi
}

install_packages_openbsd() {
    # OpenBSD's pkg_add is interactive only when ambiguous; -I picks the first.
    $SUDO pkg_add -I \
        bash \
        gcc%11 g++%11 \
        llvm \
        git \
        autoconf-2.71 automake-1.16.5 libtool gmake cmake ninja \
        unzip zip curl \
        python%3 py3-pip \
        gawk
    if [ "$SKIP_NODE" -eq 0 ]; then
        $SUDO pkg_add -I node
    fi
}

install_packages_netbsd() {
    if ! has_cmd pkgin; then
        die "pkgin is not installed. Bootstrap pkgsrc per https://www.netbsd.org/docs/pkgsrc/ and rerun."
    fi
    $SUDO pkgin -y install \
        bash \
        gcc12 \
        clang \
        git \
        pkgconf \
        autoconf automake libtool gmake cmake ninja-build \
        unzip zip curl mozilla-rootcerts \
        python311 py311-pip \
        gawk
    if [ "$SKIP_NODE" -eq 0 ]; then
        $SUDO pkgin -y install nodejs npm
    fi
}

install_packages() {
    log_step "Installing system packages"
    case "$BSD_KIND" in
        FreeBSD|DragonFly) install_packages_freebsd ;;
        OpenBSD)           install_packages_openbsd ;;
        NetBSD)            install_packages_netbsd ;;
    esac
}

install_meson() {
    log_step "Installing Meson and Ninja"

    if ! has_cmd python3; then
        die "python3 not found; rerun with --skip-meson or install Python first."
    fi

    if python3 -m pip --version >/dev/null 2>&1; then
        python3 -m pip install --user --upgrade "meson>=1.4" ninja
    else
        log_warn "pip is not available; skipping Meson/Ninja installation. Install the python pip package and rerun."
        return
    fi

    case ":${PATH}:" in
        *":${HOME}/.local/bin:"*) ;;
        *) log_warn "Add \"\$HOME/.local/bin\" to your PATH to use pip --user installs (meson, ninja)." ;;
    esac
}

verify_environment() {
    log_step "Verification"

    failed=0

    for tool in git gmake cmake pkgconf curl python3; do
        if has_cmd "$tool"; then
            log_info "$tool: $(command -v "$tool")"
        elif [ "$tool" = "pkgconf" ] && has_cmd pkg-config; then
            log_info "pkg-config: $(command -v pkg-config)"
        elif [ "$tool" = "gmake" ] && [ "$BSD_KIND" = "NetBSD" ] && has_cmd gmake; then
            log_info "gmake: $(command -v gmake)"
        else
            log_warn "$tool not found on PATH"
            failed=1
        fi
    done

    if has_cmd c++; then
        log_info "c++:    $(c++ --version 2>&1 | head -n1)"
    elif has_cmd clang++; then
        log_info "clang++: $(clang++ --version 2>&1 | head -n1)"
    elif has_cmd g++; then
        log_info "g++:    $(g++ --version 2>&1 | head -n1)"
    else
        log_warn "No C++ compiler found on PATH"
        failed=1
    fi

    if has_cmd ninja; then
        log_info "ninja:  $(ninja --version)"
    elif has_cmd samu; then
        log_info "samu:   $(samu --version 2>&1 | head -n1)"
    else
        log_warn "ninja (or samu) not found; Meson builds will fail."
    fi

    if has_cmd meson; then
        log_info "meson:  $(meson --version)"
    elif [ -x "${HOME}/.local/bin/meson" ]; then
        log_info "meson:  ${HOME}/.local/bin/meson ($(${HOME}/.local/bin/meson --version))"
    else
        log_warn "meson not found; Meson builds will not work until \$HOME/.local/bin is on PATH."
    fi

    if [ "$SKIP_NODE" -eq 0 ]; then
        if has_cmd node; then
            log_info "node:   $(node --version)"
            if ! node_version_supported; then
                log_warn "Node.js $(node --version) is outside the dashboard's supported range (${DASHBOARD_NODE_RANGE})."
                log_warn "npm install in dashboard/ will refuse it. Install a supported release, for example on FreeBSD: pkg install node24 npm-node24."
            fi
        else
            log_warn "node not found on PATH; dashboard build will not work."
        fi
        if has_cmd npm; then
            log_info "npm:    $(npm --version)"
        else
            log_warn "npm not found on PATH."
        fi
    fi

    if [ "$failed" -ne 0 ]; then
        die "One or more required tools are missing. See warnings above."
    fi
}

log_step "TheWatcher BSD build environment setup"
log_info "Repository: ${REPO_ROOT}"

detect_bsd

if [ "$SKIP_PACKAGES" -eq 0 ]; then
    install_packages
else
    log_info "Skipping package installation."
fi

if [ "$SKIP_MESON" -eq 0 ]; then
    install_meson
else
    log_info "Skipping Meson installation."
fi

if [ "$NO_VERIFY" -eq 0 ]; then
    verify_environment
fi

cat <<EOF

Setup complete.

Build with Meson:
    meson setup builddir-release --buildtype=release --default-library=static
    meson compile -C builddir-release
    meson test    -C builddir-release --print-errorlogs

Note: BSD make differs from GNU make. Use 'gmake' wherever Linux instructions
call for 'make' if invoked outside Meson.

Dashboard (needs Node.js ${DASHBOARD_NODE_RANGE}):
    cd dashboard && npm install && npm run build && npm test

If you installed Meson via pip --user, open a new shell or add
\$HOME/.local/bin to PATH before running the meson commands.
EOF
