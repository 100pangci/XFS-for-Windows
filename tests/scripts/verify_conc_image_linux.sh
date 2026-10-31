#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# CONC-1 Linux qualification for one concurrency-built image.
#
#   sudo tests/scripts/verify_conc_image_linux.sh <image> \
#       [--work DIR] [--workload mixed|shared|cow|manifest] [--threads N] \
#       [--seed N] [--iterations N] [--manifest FILE]
#
# The image is a memory-backed device exported by a deterministic
# concurrency test after a clean unmount. This script requires:
#   1. xfs_repair -n to report no corruption on the raw bytes;
#   2. the independent raw-image content oracle (cow workload);
#   3. a real kernel read-write mount whose namespace/content matches the
#      final state the stress test left behind;
#   4. xfs_scrub -n where the kernel provides the metadata scrub facility;
#   5. after unmount, a clean log and a second silent xfs_repair -n.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMG="${1:?usage: verify_conc_image_linux.sh <image> [options]}"
shift
WORK=""
WORKLOAD=""
THREADS=4
SEED=""
ITERS=""
MANIFEST=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --work) WORK="${2:?--work needs a directory}"; shift 2 ;;
    --workload) WORKLOAD="${2:?--workload needs a value}"; shift 2 ;;
    --threads) THREADS="${2:?--threads needs a value}"; shift 2 ;;
    --seed) SEED="${2:?--seed needs a value}"; shift 2 ;;
    --iterations) ITERS="${2:?--iterations needs a value}"; shift 2 ;;
    --manifest) MANIFEST="${2:?--manifest needs a file}"; shift 2 ;;
    *) echo "conc: unknown argument: $1" >&2; exit 2 ;;
  esac
done
case "$WORKLOAD" in
  ""|mixed|shared|cow|manifest) ;;
  *) echo "conc: invalid --workload $WORKLOAD" >&2; exit 2 ;;
esac
if [ "$WORKLOAD" = "manifest" ] && [ -z "$MANIFEST" ]; then
  echo "conc: --workload manifest needs --manifest FILE" >&2
  exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
  echo "conc: this script mounts a loop device; run it with sudo" >&2
  exit 2
fi
if [ ! -f "$IMG" ]; then
  echo "conc: no such image: $IMG" >&2
  exit 2
fi
if [ -z "$WORK" ]; then
  WORK="$(mktemp -d)"
fi
mkdir -p "$WORK"
MNT="$WORK/mnt"
mkdir -p "$MNT"

umount_retry() {
  local mp="$1"
  local attempt
  for attempt in 1 2 3 4 5; do
    if umount "$mp" 2>/dev/null; then
      return 0
    fi
    sleep 1
  done
  echo "conc: normal unmount failed for $mp; trying lazy unmount" >&2
  umount -l "$mp" 2>/dev/null || true
  for attempt in 1 2 3 4 5 6 7 8 9 10; do
    if ! findmnt -M "$mp" -n >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  echo "conc: $mp is still mounted after lazy unmount" >&2
  findmnt -M "$mp" || true
  if command -v fuser >/dev/null 2>&1; then
    fuser -vm "$mp" || true
  fi
  echo "conc: clean it up with: sudo umount -l '$mp'" >&2
  return 1
}

check_repair() {
  local output="$1"
  xfs_repair -n "$IMG" >"$output" 2>&1 || {
    cat "$output" >&2
    echo "conc: xfs_repair -n failed" >&2
    return 1
  }
  if grep -qiE 'corruption|fatal|would fix|ALERT|valuable metadata' \
      "$output"; then
    cat "$output" >&2
    echo "conc: xfs_repair -n found corruption" >&2
    return 1
  fi
  tail -1 "$output"
}

raw_probe_args=()
if [ -n "$SEED" ]; then raw_probe_args+=(--seed "$SEED"); fi
if [ -n "$ITERS" ]; then raw_probe_args+=(--iterations "$ITERS"); fi

# 1. Raw bytes first: no kernel write may be needed to make the image valid.
check_repair "$WORK/xfs_repair_pre.txt"

# 2. The independent raw oracle for the workload's known final contents.
if [ "$WORKLOAD" = "cow" ]; then
  python3 -I "$ROOT/tests/tools/conc_cow_probe.py" "$IMG" \
    "${raw_probe_args[@]}" | tee "$WORK/conc_raw.txt"
elif [ "$WORKLOAD" = "mixed" ] || [ "$WORKLOAD" = "shared" ]; then
  python3 -I "$ROOT/tests/tools/conc_mount_probe.py" --image "$IMG" \
    --workload "$WORKLOAD" --threads "$THREADS" | tee "$WORK/conc_raw.txt"
fi

# 3. A writable kernel mount observes the same namespace and contents.
mount -o loop,nouuid "$IMG" "$MNT"
trap 'umount_retry "$MNT" 2>/dev/null || true' EXIT
if [ -n "$WORKLOAD" ]; then
  mount_args=(--mount "$MNT" --workload "$WORKLOAD" --threads "$THREADS")
  if [ -n "$SEED" ]; then mount_args+=(--seed "$SEED"); fi
  if [ -n "$ITERS" ]; then mount_args+=(--iterations "$ITERS"); fi
  if [ -n "$MANIFEST" ]; then mount_args+=(--manifest "$MANIFEST"); fi
  python3 -I "$ROOT/tests/tools/conc_mount_probe.py" "${mount_args[@]}"
fi
find "$MNT" -maxdepth 8 -printf '%y %n %s %p -> %l\n' | sort \
  >"$WORK/find.txt"

# 4. Online metadata scrub (read-only check).
if command -v xfs_scrub >/dev/null 2>&1; then
  if ! xfs_scrub -n "$MNT" 2>&1 | tee "$WORK/xfs_scrub.txt"; then
    if grep -q 'Kernel metadata scrubbing facility is not available' \
        "$WORK/xfs_scrub.txt"; then
      echo "conc: kernel has no online scrub ioctl; recording a skip" \
        | tee -a "$WORK/xfs_scrub.txt"
    else
      echo "conc: xfs_scrub rejected the image" >&2
      exit 1
    fi
  fi
else
  echo "conc: xfs_scrub is not installed; recording an online-scrub skip" \
    | tee "$WORK/xfs_scrub.txt"
fi
umount_retry "$MNT"
trap - EXIT

# 5. Clean unmount and a second structural check of the post-mount bytes.
if command -v xfs_logprint >/dev/null 2>&1; then
  xfs_logprint -t "$IMG" >"$WORK/xfs_logprint.txt" 2>&1
  grep -q 'state: <CLEAN>' "$WORK/xfs_logprint.txt" || {
    tail -20 "$WORK/xfs_logprint.txt" >&2
    echo "conc: the log is not clean after unmount" >&2
    exit 1
  }
else
  python3 -I "$ROOT/tests/tools/log_probe.py" "$IMG" --expect clean \
    >"$WORK/log_probe.txt"
fi
check_repair "$WORK/xfs_repair_post.txt"
if [ "$WORKLOAD" = "cow" ]; then
  python3 -I "$ROOT/tests/tools/conc_cow_probe.py" "$IMG" \
    "${raw_probe_args[@]}" --quiet
fi
echo "conc: Linux verification passed ($(basename "$IMG"), workload=${WORKLOAD:-structural})"
