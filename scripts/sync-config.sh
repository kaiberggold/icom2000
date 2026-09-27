#!/usr/bin/env bash
# Copies the repo's runtime config to a Pi (config/README.md has what goes
# where):
#
#   config/icom2000.conf -> /etc/icom2000.conf
#   config/asound.conf   -> /etc/asound.conf
#
# Before replacing a file it keeps the old one next to it, timestamped:
# /etc/asound.conf.bak-20260927-143005. Files that are already identical
# are left alone (no backup, no copy). Uses sudo on the Pi.
set -euo pipefail

usage() {
    echo "usage: $(basename "$0") <user@host>" >&2
    echo "  or:  ICOM2000_PI=<user@host> $(basename "$0")" >&2
}

TARGET="${1:-${ICOM2000_PI:-}}"
if [[ -z "${TARGET}" || "${TARGET}" == -* ]]; then
    usage
    [[ "${TARGET}" == "-h" || "${TARGET}" == "--help" ]] && exit 0
    exit 2
fi

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FILES=(icom2000.conf asound.conf)
# The dev machine's clock, not the Pi's: a Pi Zero has no battery-backed
# clock, so right after boot without network its time can be far off.
STAMP="$(date +%Y%m%d-%H%M%S)"

STAGING="$(mktemp -d)"
trap 'rm -rf "${STAGING}"' EXIT
for f in "${FILES[@]}"; do
    cp "${REPO_DIR}/config/${f}" "${STAGING}/"
done

# Runs on the Pi, from the directory it was copied to.
cat > "${STAGING}/apply.sh" <<EOF
set -euo pipefail
cd "\$(dirname "\$0")"
for f in ${FILES[*]}; do
    dst="/etc/\${f}"
    if [[ -e "\${dst}" ]] && cmp -s "\${f}" "\${dst}"; then
        echo "    \${dst}: unchanged"
        continue
    fi
    if [[ -e "\${dst}" ]]; then
        sudo cp -p "\${dst}" "\${dst}.bak-${STAMP}"
        echo "    \${dst}: old version kept as \${dst}.bak-${STAMP}"
    fi
    sudo install -m 0644 "\${f}" "\${dst}"
    echo "    \${dst}: updated"
done
rm -rf "\$(pwd)"
EOF

echo "==> copying config to ${TARGET}"
REMOTE_TMP="$(ssh "${TARGET}" 'mktemp -d')"
scp -q "${STAGING}"/* "${TARGET}:${REMOTE_TMP}/"
# -t: sudo may need to ask for a password.
ssh -t "${TARGET}" "bash ${REMOTE_TMP}/apply.sh"
echo "==> done -- restart intercomd / icom-audiotest to pick up the changes"
