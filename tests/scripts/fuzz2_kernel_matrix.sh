#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# FUZZ-2 kernel crash matrix wrapper.
#
#   sudo bash tests/scripts/fuzz2_kernel_matrix.sh
#
# Runs tests/scripts/fuzz2_crash_matrix.sh with kernel qualification enabled
# and a persistent log.  The matrix itself applies every unmount fallback and
# the exit-trap cleanup documented in docs/STATEFUL_FUZZING.md.
#
# Environment overrides (same names as fuzz2_crash_matrix.sh, plus LOG):
#   EXE            stateful_fuzz binary (default <repo>/build/tests/stateful_fuzz)
#   WORK           matrix work directory (default /var/tmp/fuzz2-kernel-$WORKLOAD)
#   SEED STEPS CRASH_DENSITY PROFILE WORKLOAD SIZE_MB KEEP_IMAGES
#   LOG            transcript (default /tmp/fuzz2-kernel-$WORKLOAD.log)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXE="${EXE:-$ROOT/build/tests/stateful_fuzz}"
WORKLOAD="${WORKLOAD:-mixed}"
WORK="${WORK:-/var/tmp/fuzz2-kernel-$WORKLOAD}"
SEED="${SEED:-0x123456789abcdef}"
STEPS="${STEPS:-80}"
CRASH_DENSITY="${CRASH_DENSITY:-1}"
PROFILE="${PROFILE:-modern}"
SIZE_MB="${SIZE_MB:-300}"
KEEP_IMAGES="${KEEP_IMAGES:-0}"
LOG="${LOG:-/tmp/fuzz2-kernel-$WORKLOAD.log}"

if [ "$(id -u)" -ne 0 ]; then
  echo "fuzz2: run this script with sudo (kernel verification mounts loop devices)" >&2
  exit 2
fi
if [ ! -x "$EXE" ]; then
  echo "fuzz2: missing executable $EXE; build with -DXFSWIN_BUILD_TESTS=ON" >&2
  exit 1
fi

leaked_mounts="$(findmnt -rn -o TARGET 2>/dev/null |
  awk -v p="$WORK/" 'index($0, p) == 1')"
if [ -n "$leaked_mounts" ]; then
  echo "fuzz2: $WORK still has mounted targets; unmount them first:" >&2
  printf '%s\n' "$leaked_mounts" >&2
  exit 1
fi
if losetup -a 2>/dev/null | grep -Fq "$WORK/"; then
  echo "fuzz2: loop devices still reference $WORK; detach them first:" >&2
  losetup -a 2>/dev/null | grep -F "$WORK/" >&2 || true
  exit 1
fi
mkdir -p "$(dirname "$LOG")"

echo "fuzz2: workload=$WORKLOAD seed=$SEED steps=$STEPS density=$CRASH_DENSITY"
echo "fuzz2: profile=$PROFILE size=${SIZE_MB}MiB work=$WORK"
echo "fuzz2: log=$LOG"

if ! env EXE="$EXE" WORK="$WORK" SEED="$SEED" STEPS="$STEPS" \
    CRASH_DENSITY="$CRASH_DENSITY" PROFILE="$PROFILE" \
    WORKLOAD="$WORKLOAD" SIZE_MB="$SIZE_MB" KEEP_IMAGES="$KEEP_IMAGES" \
    bash "$ROOT/tests/scripts/fuzz2_crash_matrix.sh" 2>&1 | tee "$LOG"; then
  echo "fuzz2: crash matrix FAILED; log: $LOG" >&2
  echo "fuzz2: artifacts (if any) are under $WORK" >&2
  exit 1
fi

echo "fuzz2: crash matrix passed; log: $LOG"
