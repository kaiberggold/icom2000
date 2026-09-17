#!/usr/bin/env bash
# Copies the cross-built binaries to a Pi Zero and restarts the service.
# Assumes: `cmake --build --preset pi0-release` already ran, and the target
# already has systemd/intercomd.service installed & enabled, and
# udev/99-icom2000-gpio.rules installed (see docs/ARCHITECTURE.md
# "Deploying"). This script only pushes the binaries and bounces the unit.
set -euo pipefail

usage() {
    echo "usage: $(basename "$0") <user@host>" >&2
    echo "  or:  ICOM2000_PI=<user@host> $(basename "$0")" >&2
}

TARGET="${1:-${ICOM2000_PI:-}}"
if [[ -z "${TARGET}" ]]; then
    usage
    exit 2
fi

BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/pi0-release"
DAEMON_BIN="${BUILD_DIR}/src/app/intercomd"
CLI_BIN="${BUILD_DIR}/src/cli/intercomctl"

for f in "${DAEMON_BIN}" "${CLI_BIN}"; do
    if [[ ! -x "${f}" ]]; then
        echo "error: ${f} not found -- build the pi0-release preset first:" >&2
        echo "  cmake --preset pi0-release && cmake --build --preset pi0-release" >&2
        exit 1
    fi
done

echo "==> copying binaries to ${TARGET}:/tmp"
scp "${DAEMON_BIN}" "${CLI_BIN}" "${TARGET}:/tmp/"

echo "==> installing and restarting intercomd on ${TARGET}"
ssh "${TARGET}" '
    set -euo pipefail
    sudo install -m 0755 /tmp/intercomd  /usr/local/bin/intercomd
    sudo install -m 0755 /tmp/intercomctl /usr/local/bin/intercomctl
    rm -f /tmp/intercomd /tmp/intercomctl
    sudo systemctl restart intercomd
    sudo systemctl --no-pager status intercomd
'
