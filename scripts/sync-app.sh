#!/usr/bin/env bash
# Copies the current Pi build to a Pi without starting anything -- the copy
# step of the VS Code "Debug intercomd on Pi Zero" session on its own:
# intercomd lands as <dir>/intercomd-debug (the name that session runs),
# next to intercomctl and icom-audiotest. Doesn't build first: run
# `cmake --build --preset pi0-debug` (or the matching VS Code task) before.
set -euo pipefail

usage() {
    echo "usage: $(basename "$0") <user@host> [--dir DIR] [--preset NAME]" >&2
    echo "  or:  ICOM2000_PI=<user@host> $(basename "$0") ..." >&2
    echo "  --dir DIR      directory on the Pi (default: the user's home)" >&2
    echo "  --preset NAME  build preset to copy from (default: pi0-debug)" >&2
}

TARGET="${ICOM2000_PI:-}"
REMOTE_DIR=""
PRESET="pi0-debug"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dir) REMOTE_DIR="${2:?--dir needs a directory}"; shift 2 ;;
        --preset) PRESET="${2:?--preset needs a name}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        -*) usage; exit 2 ;;
        *) TARGET="$1"; shift ;;
    esac
done
if [[ -z "${TARGET}" ]]; then
    usage
    exit 2
fi

BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build/${PRESET}"
DAEMON_BIN="${BUILD_DIR}/src/app/intercomd"
TOOL_BINS=("${BUILD_DIR}/src/cli/intercomctl" "${BUILD_DIR}/src/cli/icom-audiotest")

for f in "${DAEMON_BIN}" "${TOOL_BINS[@]}"; do
    if [[ ! -x "${f}" ]]; then
        echo "error: ${f} not found -- build the ${PRESET} preset first:" >&2
        echo "  cmake --build --preset ${PRESET}" >&2
        exit 1
    fi
done

DEST="${TARGET}:${REMOTE_DIR:+${REMOTE_DIR%/}/}"
echo "==> copying the ${PRESET} build to ${DEST}"
# A running copy (e.g. under gdbserver) makes this fail with "Text file
# busy" -- stop the debug session first.
scp -q "${DAEMON_BIN}" "${DEST}intercomd-debug"
scp -q "${TOOL_BINS[@]}" "${DEST}"
echo "    intercomd-debug, intercomctl, icom-audiotest"
