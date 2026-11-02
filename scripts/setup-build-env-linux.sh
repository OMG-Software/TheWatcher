#!/usr/bin/env bash
# Set up the TheWatcher build environment on Linux.
#
# Installs the Meson + Ninja toolchain, the C++20 compiler stack, and Node.js
# for the dashboard. Detects the distribution from /etc/os-release and uses
# the matching package manager.
#
# Supported distributions:
#   - Debian / Ubuntu (apt-get)
#   - Fedora / RHEL / CentOS / Rocky / Alma (dnf, falling back to yum)
#   - Arch / Manjaro (pacman)
#   - openSUSE / SLES (zypper)
#   - Alpine (apk)
#
# Usage:
#   ./scripts/setup-build-env-linux.sh [options]
#
# Options:
#   --skip-packages     Do not install distro packages.
#   --skip-meson        Do not install Meson/Ninja via pip.
#   --skip-node         Do not install Node.js/npm (dashboard build).
#   --no-verify         Skip the post-install verification step.
#   -h, --help          Show this help and exit.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
        --skip-packages)   SKIP_PACKAGES=1 ;;
        --skip-meson)      SKIP_MESON=1 ;;
        --skip-node)       SKIP_NODE=1 ;;
        --no-verify)       NO_VERIFY=1 ;;
        -h|--help)         usage; exit 0 ;;
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
    if has_cmd sudo; then
        SUDO="sudo"
    else
        die "This script needs root privileges or sudo to install packages."
    fi
fi

detect_distro() {
    if [ -r /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        DISTRO_ID="${ID:-unknown}"
        DISTRO_LIKE="${ID_LIKE:-}"
    else
        DISTRO_ID="unknown"
        DISTRO_LIKE=""
    fi

    case "${DISTRO_ID}:${DISTRO_LIKE}" in
        debian:*|ubuntu:*|linuxmint:*|*:*debian*|*:*ubuntu*)
            PKG_FAMILY="debian" ;;
        fedora:*|rhel:*|centos:*|rocky:*|almalinux:*|*:*rhel*|*:*fedora*)
            PKG_FAMILY="rhel" ;;
        arch:*|manjaro:*|endeavouros:*|*:*arch*)
            PKG_FAMILY="arch" ;;
        opensuse*:*|sles:*|*:*suse*)
            PKG_FAMILY="suse" ;;
        alpine:*)
            PKG_FAMILY="alpine" ;;
        *)
            die "Unsupported distribution: ID=${DISTRO_ID} ID_LIKE=${DISTRO_LIKE}. Re-run with --skip-packages and install dependencies manually." ;;
    esac

    log_info "Detected distribution: ${DISTRO_ID} (family: ${PKG_FAMILY})"
}

install_packages_debian() {
    export DEBIAN_FRONTEND=noninteractive
    $SUDO apt-get update
    $SUDO apt-get install -y \
        build-essential clang lld \
        git pkg-config \
        autoconf automake libtool make cmake ninja-build \
        unzip zip curl ca-certificates \
        python3 python3-pip python3-venv \
        gawk
    # pipx is shipped on Debian 12+ / Ubuntu 22.04+; best-effort.
    $SUDO apt-get install -y pipx || log_warn "pipx package unavailable; will fall back to a managed venv for meson/ninja."
    if [ "$SKIP_NODE" -eq 0 ]; then
        $SUDO apt-get install -y nodejs npm
    fi
}

install_packages_rhel() {
    if has_cmd dnf; then
        PKG="$SUDO dnf install -y"
    elif has_cmd yum; then
        PKG="$SUDO yum install -y"
    else
        die "Neither dnf nor yum is available."
    fi
    $PKG gcc gcc-c++ clang lld \
        git pkgconfig \
        autoconf automake libtool make cmake ninja-build \
        unzip zip curl ca-certificates \
        python3 python3-pip \
        gawk
    $PKG pipx || log_warn "pipx package unavailable; will fall back to a managed venv for meson/ninja."
    if [ "$SKIP_NODE" -eq 0 ]; then
        $PKG nodejs npm
    fi
}

install_packages_arch() {
    $SUDO pacman -Sy --noconfirm --needed \
        base-devel clang lld \
        git pkgconf \
        autoconf automake libtool make cmake ninja \
        unzip zip curl ca-certificates \
        python python-pip python-pipx \
        gawk
    if [ "$SKIP_NODE" -eq 0 ]; then
        $SUDO pacman -S --noconfirm --needed nodejs npm
    fi
}

install_packages_suse() {
    $SUDO zypper --non-interactive install -y \
        gcc gcc-c++ clang lld \
        git pkg-config \
        autoconf automake libtool make cmake ninja \
        unzip zip curl ca-certificates \
        python3 python3-pip \
        gawk
    $SUDO zypper --non-interactive install -y python3-pipx \
        || log_warn "python3-pipx package unavailable; will fall back to a managed venv for meson/ninja."
    if [ "$SKIP_NODE" -eq 0 ]; then
        $SUDO zypper --non-interactive install -y nodejs npm
    fi
}

install_packages_alpine() {
    $SUDO apk add --no-cache \
        build-base clang lld \
        git pkgconf \
        autoconf automake libtool make cmake samurai \
        unzip zip curl ca-certificates \
        bash python3 py3-pip \
        gawk linux-headers
    $SUDO apk add --no-cache pipx || log_warn "pipx package unavailable; will fall back to a managed venv for meson/ninja."
    if [ "$SKIP_NODE" -eq 0 ]; then
        $SUDO apk add --no-cache nodejs npm
    fi
}

install_packages() {
    log_step "Installing system packages"
    case "$PKG_FAMILY" in
        debian) install_packages_debian ;;
        rhel)   install_packages_rhel ;;
        arch)   install_packages_arch ;;
        suse)   install_packages_suse ;;
        alpine) install_packages_alpine ;;
    esac
}

install_meson() {
    log_step "Installing Meson and Ninja"

    if ! has_cmd python3; then
        die "python3 not found; rerun with --skip-meson or install Python first."
    fi

    if has_cmd meson; then
        local current
        current="$(meson --version 2>/dev/null || true)"
        log_info "Existing meson: ${current:-unknown}"
    fi

    # Modern distros (Debian 12+, Ubuntu 23.04+, Fedora 38+, Arch, ...) mark the
    # system Python as externally managed (PEP 668), so `pip install --user`
    # fails. Prefer pipx (one isolated venv per app); fall back to a managed
    # venv we own, then to `pip --break-system-packages` as a last resort.
    if has_cmd pipx; then
        log_info "Using pipx"
        pipx install --force "meson>=1.4"
        pipx install --force ninja
    else
        local venv_dir="${HOME}/.local/share/thewatcher-pyenv"
        log_info "pipx unavailable; creating managed venv at ${venv_dir}"
        if ! python3 -m venv "${venv_dir}"; then
            die "Failed to create venv at ${venv_dir}. Install python3-venv (or your distro's equivalent) and rerun, or use --skip-meson."
        fi
        "${venv_dir}/bin/pip" install --upgrade pip
        "${venv_dir}/bin/pip" install --upgrade "meson>=1.4" ninja

        mkdir -p "${HOME}/.local/bin"
        ln -sf "${venv_dir}/bin/meson" "${HOME}/.local/bin/meson"
        ln -sf "${venv_dir}/bin/ninja" "${HOME}/.local/bin/ninja"
    fi

    case ":${PATH}:" in
        *":${HOME}/.local/bin:"*) ;;
        *) log_warn "Add \"\$HOME/.local/bin\" to your PATH to pick up meson and ninja." ;;
    esac
}

verify_environment() {
    log_step "Verification"

    local failed=0

    for tool in git make cmake pkg-config curl python3; do
        if has_cmd "$tool"; then
            log_info "$tool: $(command -v "$tool")"
        else
            log_warn "$tool not found on PATH"
            failed=1
        fi
    done

    if has_cmd g++; then
        log_info "g++:    $(g++ --version | head -n1)"
    elif has_cmd clang++; then
        log_info "clang++: $(clang++ --version | head -n1)"
    else
        log_warn "No C++ compiler (g++ or clang++) found on PATH"
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
                log_warn "npm install in dashboard/ will refuse it. Distribution packages are often too old (Ubuntu 24.04 ships Node.js 18); install a supported release from https://nodejs.org/en/download (NodeSource packages or nvm)."
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

log_step "TheWatcher Linux build environment setup"
log_info "Repository: ${REPO_ROOT}"

detect_distro

if [ "$SKIP_PACKAGES" -eq 0 ]; then
    install_packages
else
    log_info "Skipping system package installation."
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

Dashboard (needs Node.js ${DASHBOARD_NODE_RANGE}):
    cd dashboard && npm install && npm run build && npm test

If you installed Meson via pip --user, open a new shell or add
\$HOME/.local/bin to PATH before running the meson commands.
EOF
