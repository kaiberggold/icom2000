#!/usr/bin/env bash
# Enforces the architecture invariants from the "future-proofing for the
# network extension" requirements pass -- see docs/ARCHITECTURE.md
# "Architecture invariants" for what each one means and why. Also
# registered as a ctest test (tests/CMakeLists.txt) so `ctest` runs this
# on every build, not just when someone remembers to run it by hand.
#
# Exits nonzero and prints the offending lines on any violation.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."

status=0

section() { echo "== $1 =="; }

section "no hardcoded ALSA card names outside config/ (requirement 1)"
# Quote-anchored so this doesn't also match "icom::hw::" -- a real
# violation is always a C++ string literal ("hw:Zero", "plughw:Zero", ...).
if grep -rnE '"(plug)?hw:' src/; then
    echo "FAIL: found a literal ALSA card name in application code -- route through" >&2
    echo "      a named PCM device from config/asound.conf instead." >&2
    status=1
else
    echo "OK"
fi

section "no direct mixer manipulation outside boot-time restore (requirement 2)"
if grep -rnE '\bamixer\b|\bsnd_mixer_' src/; then
    echo "FAIL: mixer state must be set exclusively at boot -- see" >&2
    echo "      systemd/alsa-restore-codec-zero.service -- never scattered calls" >&2
    echo "      from the application at runtime." >&2
    status=1
else
    echo "OK"
fi

section "hw/ and gpio/ layers stay audio-agnostic (requirement 4)"
if grep -rnE '#include "icom/audio/' src/hw src/gpio 2>/dev/null; then
    echo "FAIL: src/hw and src/gpio must never depend on src/audio -- physical" >&2
    echo "      triggers are decoupled from whatever reacts to them; wire them" >&2
    echo "      together only in the composition root (src/app)." >&2
    status=1
else
    echo "OK"
fi

exit "$status"
