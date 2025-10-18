#!/usr/bin/env bash
set -euo pipefail

# codex_env.sh - helper to bootstrap a local build environment that mirrors the
# one used by the Codex automation container.
#
# The script can be sourced to export useful defaults, or executed to perform
# dependency installation and environment validation.

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CODEX_PACKAGES=(build-essential libsdl2-dev pkg-config)

usage() {
    cat <<USAGE
Usage: ${0##*/} [--install-deps] [--configure]

Options:
  --install-deps  Install build prerequisites using apt-get (requires root).
  --configure     Run the project's ./configure script after preparing the
                  environment.

Run the script without arguments to only export recommended environment
variables. When sourcing, the installation and configure steps are skipped.
USAGE
}

run_install_deps() {
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "apt-get is not available on this system. Install the following packages manually:" >&2
        printf '  - %s\n' "${CODEX_PACKAGES[@]}" >&2
        return 1
    fi

    echo "Installing build dependencies via apt-get..."
    apt-get update
    apt-get install -y "${CODEX_PACKAGES[@]}"
}

run_configure() {
    echo "Running project configure script..."
    (cd "$PROJECT_ROOT" && ./configure)
}

# Export default toolchain environment variables.
export CC="${CC:-gcc}"
export CFLAGS="${CFLAGS:--O2 -g}"
export CXXFLAGS="${CXXFLAGS:-$CFLAGS}"
export LDFLAGS="${LDFLAGS:-}"
export PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}"  # Allow callers to override.

if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then
    # Script is being sourced; do not execute side effects.
    return 0
fi

INSTALL_DEPS=0
RUN_CONFIGURE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-deps)
            INSTALL_DEPS=1
            shift
            ;;
        --configure)
            RUN_CONFIGURE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ $INSTALL_DEPS -eq 1 ]]; then
    run_install_deps
fi

if [[ $RUN_CONFIGURE -eq 1 ]]; then
    run_configure
fi

# If we reach here the script executed successfully.
echo "Codex environment setup complete."
