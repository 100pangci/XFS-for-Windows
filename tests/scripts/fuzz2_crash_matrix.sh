#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# FUZZ-2 crash matrix driver.
#
#   sudo EXE=build/tests/stateful_fuzz tests/scripts/fuzz2_crash_matrix.sh
#
# For each scenario the script chooses a deterministic operation from the
# generated plan, crashes at every applicable injection point, verifies the
# pre-recovery image through a real kernel replay, then recovers it through the
# project engine and verifies the recovered image again.  Set SKIP_LINUX=1 on a
# host without mount privileges to run only the C++ recovery checks.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXE="${EXE:-$ROOT/build/tests/stateful_fuzz}"
WORK="${WORK:-$(mktemp -d)}"
SEED="${SEED:-0x123456789abcdef}"
STEPS="${STEPS:-80}"
SKIP_LINUX="${SKIP_LINUX:-0}"
SIZE_MB="${SIZE_MB:-300}"
mkdir -p "$WORK"

if [ ! -x "$EXE" ]; then
  echo "fuzz2: missing executable $EXE" >&2
  exit 1
fi

plan="$WORK/plan.txt"
"$EXE" --seed "$SEED" --steps "$STEPS" --image "$WORK/plan.img" \
  --profile modern --workload mixed --plan-only "$plan" >/dev/null 2>&1

choose_op() {
  local type="$1"
  awk -v type="$type" '$2 == type { print $1; exit }' "$plan"
}

namespace_op="$(choose_op rmdir)"
if [ -z "$namespace_op" ]; then namespace_op="$(choose_op mkdir)"; fi
data_op="$(choose_op write)"
if [ -z "$namespace_op" ] || [ -z "$data_op" ]; then
  echo "fuzz2: could not find the crash operations in $plan" >&2
  exit 1
fi

failures=0
run_case() {
  local name="$1" op="$2" point="$3"
  local base="$WORK/$name-$point"
  local image="$base.img"
  local trace="$base.trace.txt"
  local manifest="$base.manifest.json"
  rm -f "$image" "$trace" "$manifest" "$base.pre.img" "$base.rec.img" \
    "$base.log" "$base.recover.log"

  echo "== $name op=$op point=$point"
  set +e
  "$EXE" --seed "$SEED" --steps "$STEPS" --image "$image" --trace "$trace" \
    --manifest "$manifest" --profile modern --workload mixed \
    --image-size-mb "$SIZE_MB" --crash-op "$op" --crash-point "$point" \
    --check-every 10 --consistency-every 0 >"$base.log" 2>&1
  local rc=$?
  set -e
  if [ "$rc" -ne 72 ]; then
    echo "fuzz2: crash point $point did not terminate (exit $rc)" >&2
    tail -20 "$base.log" >&2
    failures=$((failures + 1))
    return
  fi
  cp "$image" "$base.pre.img"

  "$EXE" --seed "$SEED" --steps "$STEPS" --image "$image" --trace "$trace" \
    --manifest "$manifest" --profile modern --workload mixed \
    --crash-op "$op" --crash-point "$point" --recover \
    >"$base.recover.log" 2>&1
  cp "$image" "$base.rec.img"

  if [ "$SKIP_LINUX" -eq 0 ]; then
    bash "$ROOT/tests/scripts/verify_stateful_linux.sh" \
      "$base.pre.img" "$manifest" "$base.pre.work" --kernel-replay
    bash "$ROOT/tests/scripts/verify_stateful_linux.sh" \
      "$base.rec.img" "$manifest" "$base.rec.work"
  fi
  echo "== $name op=$op point=$point OK"
  if [ "${KEEP_IMAGES:-0}" -eq 0 ]; then
    rm -f "$base.pre.img" "$base.rec.img"
    rm -rf "$base.pre.work" "$base.rec.work"
    rm -f "$image" "$trace" "$manifest" "$base.log" "$base.recover.log"
  fi
}

# Transactional namespace: old-or-new, never a partial dirent/nlink/parent.
for point in before_log_write after_log_commit before_metadata_write \
             after_metadata_write; do
  run_case namespace "$namespace_op" "$point"
done

# Data writes have the weaker documented contract: size old/new, each byte
# old/new (plus zero fill on a newly visible range).
for point in before_data_write after_data_write before_log_write \
             after_log_commit before_metadata_write after_metadata_write; do
  run_case data "$data_op" "$point"
done

if [ "$failures" -ne 0 ]; then
  echo "fuzz2: $failures crash cases failed; artifacts in $WORK" >&2
  exit 1
fi
echo "fuzz2: crash matrix passed ($WORK)"
