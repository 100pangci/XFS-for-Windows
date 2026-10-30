#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# FUZZ-2 long-running soak driver.
#
#   sudo bash tests/scripts/fuzz2_soak.sh
#   SKIP_LINUX=1 bash tests/scripts/fuzz2_soak.sh     # non-root loop smoke
#
# Runs the deterministic stateful generator for every (seed, workload) pair,
# verifies each clean image with the Linux/xfsprogs oracles unless SKIP_LINUX=1,
# and keeps a resumable per-pair record.  A successful pair gets <base>.pass and
# its image is deleted unless KEEP_IMAGES=1; a failure keeps the image, trace,
# manifest and log for the reproducer and stops unless FAIL_FAST=0.
#
# Environment:
#   EXE         stateful_fuzz binary (default <repo>/build/tests/stateful_fuzz)
#   WORK        output directory (default /var/tmp/fuzz2-soak)
#   SEEDS       space-separated seeds (default three fixed seeds)
#   WORKLOADS   space-separated workloads (default mixed dir xattr alloc)
#   STEPS       operations per sequence (default 2000)
#   PROFILE     image profile (default modern)
#   SIZE_MB     image size (default 512)
#   CHECK_EVERY in-process model check interval (default 50)
#   SKIP_LINUX  1 = run only the project engine (default 0)
#   KEEP_IMAGES 1 = keep successful images too (default 0)
#   RESUME      1 = skip pairs with an existing .pass marker (default 1)
#   FAIL_FAST   1 = stop at the first failure (default 1)
#   RUN_TIMEOUT seconds per sequence, 0 = none (default 0)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXE="${EXE:-$ROOT/build/tests/stateful_fuzz}"
WORK="${WORK:-/var/tmp/fuzz2-soak}"
SEEDS="${SEEDS:-0x1111111111111111 0x2222222222222222 0x3333333333333333}"
WORKLOADS="${WORKLOADS:-mixed dir xattr alloc}"
STEPS="${STEPS:-2000}"
PROFILE="${PROFILE:-modern}"
SIZE_MB="${SIZE_MB:-512}"
CHECK_EVERY="${CHECK_EVERY:-50}"
SKIP_LINUX="${SKIP_LINUX:-0}"
KEEP_IMAGES="${KEEP_IMAGES:-0}"
RESUME="${RESUME:-1}"
FAIL_FAST="${FAIL_FAST:-1}"
RUN_TIMEOUT="${RUN_TIMEOUT:-0}"

if [ "$SKIP_LINUX" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
  echo "fuzz2: kernel verification needs root; run with sudo or set SKIP_LINUX=1" >&2
  exit 2
fi
if [ ! -x "$EXE" ]; then
  echo "fuzz2: missing executable $EXE; build with -DXFSWIN_BUILD_TESTS=ON" >&2
  exit 1
fi
if [ -z "$SEEDS" ] || [ -z "$WORKLOADS" ]; then
  echo "fuzz2: SEEDS and WORKLOADS must not be empty" >&2
  exit 2
fi
for value in "$STEPS" "$SIZE_MB" "$CHECK_EVERY" "$SKIP_LINUX" \
             "$KEEP_IMAGES" "$RESUME" "$FAIL_FAST" "$RUN_TIMEOUT"; do
  if [[ ! "$value" =~ ^[0-9]+$ ]]; then
    echo "fuzz2: numeric option expected, got '$value'" >&2
    exit 2
  fi
done

mkdir -p "$WORK"
SUMMARY="$WORK/summary.txt"
{
  echo "FUZZ-2 soak"
  echo "exe=$EXE"
  echo "steps=$STEPS profile=$PROFILE size=${SIZE_MB}MiB check_every=$CHECK_EVERY"
  echo "seeds=$SEEDS"
  echo "workloads=$WORKLOADS"
  echo "skip_linux=$SKIP_LINUX keep_images=$KEEP_IMAGES resume=$RESUME"
} >>"$SUMMARY"

passed=0
failed=0
skipped=0

run_one() {
  local seed="$1"
  local workload="$2"
  local base="$WORK/$workload-$seed"
  local image="$base.img"
  local trace="$base.trace.txt"
  local manifest="$base.manifest.json"
  local log="$base.log"

  if [ "$RESUME" -eq 1 ] && [ -f "$base.pass" ]; then
    echo "== SKIP (passed earlier): $workload seed=$seed"
    skipped=$((skipped + 1))
    return 0
  fi
  rm -f "$base.pass" "$base.fail"

  echo "== $workload seed=$seed steps=$STEPS"
  local rc=0
  local cmd=("$EXE" --seed "$seed" --steps "$STEPS" --image "$image"
             --trace "$trace" --manifest "$manifest" --profile "$PROFILE"
             --workload "$workload" --image-size-mb "$SIZE_MB"
             --check-every "$CHECK_EVERY" --consistency-every 0 --quiet)
  if [ "$RUN_TIMEOUT" -gt 0 ]; then
    timeout "$RUN_TIMEOUT" "${cmd[@]}" >"$log" 2>&1 || rc=$?
  else
    "${cmd[@]}" >"$log" 2>&1 || rc=$?
  fi
  if [ "$rc" -ne 0 ]; then
    echo "fuzz2: FAILED $workload seed=$seed (rc=$rc); log: $log" >&2
    tail -40 "$log" >&2 || true
    printf 'FAIL workload=%s seed=%s steps=%s rc=%s\n' \
      "$workload" "$seed" "$STEPS" "$rc" >>"$SUMMARY"
    touch "$base.fail"
    return 1
  fi

  if [ "$SKIP_LINUX" -eq 0 ]; then
    if ! bash "$ROOT/tests/scripts/verify_stateful_linux.sh" \
        "$image" "$manifest" "$base.work" \
        >"$base.verify.log" 2>&1; then
      echo "fuzz2: LINUX VERIFY FAILED $workload seed=$seed;" \
           "log: $base.verify.log" >&2
      tail -40 "$base.verify.log" >&2 || true
      printf 'VERIFY-FAIL workload=%s seed=%s steps=%s\n' \
        "$workload" "$seed" "$STEPS" >>"$SUMMARY"
      touch "$base.fail"
      return 1
    fi
  fi

  touch "$base.pass"
  printf 'PASS workload=%s seed=%s steps=%s\n' \
    "$workload" "$seed" "$STEPS" >>"$SUMMARY"
  passed=$((passed + 1))
  if [ "$KEEP_IMAGES" -eq 0 ]; then
    rm -f "$image"
  fi
  return 0
}

for seed in $SEEDS; do
  for workload in $WORKLOADS; do
    if ! run_one "$seed" "$workload"; then
      failed=$((failed + 1))
      if [ "$FAIL_FAST" -eq 1 ]; then
        echo "fuzz2: stopping at the first failure; artifacts in $WORK" >&2
        echo "fuzz2: soak summary: passed=$passed failed=$failed skipped=$skipped" >&2
        exit 1
      fi
    fi
  done
done

echo "fuzz2: soak summary: passed=$passed failed=$failed skipped=$skipped"
echo "fuzz2: summary: $SUMMARY"
if [ "$failed" -ne 0 ]; then
  exit 1
fi
