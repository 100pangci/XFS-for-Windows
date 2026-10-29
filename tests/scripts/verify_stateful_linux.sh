#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# FUZZ-2 Linux qualification for one stateful image.
#
#   sudo tests/scripts/verify_stateful_linux.sh <image> <manifest> [workdir] [--kernel-replay]
#
# With --kernel-replay the image is a pre-recovery crash image: the raw
# semantic probe is run only after the kernel has replayed the log, never
# against the torn pre-replay contents.
#
# The image may be a cleanly closed sequence, a project-recovered crash image,
# or a pre-recovery crash image (the kernel then performs the replay).  The
# script requires:
#   1. xfs_repair -n to report no corruption;
#   2. the independent raw-image semantic oracle (stateful_probe.py);
#   3. the independent parent-pointer oracle on modern images;
#   4. a real kernel mount whose namespace/content/xattrs match the manifest;
#   5. after unmount, a clean log and a second silent xfs_repair -n;
#   6. xfs_scrub -n where the kernel provides the metadata scrub facility.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMG="${1:?usage: verify_stateful_linux.sh <image> <manifest> [workdir]}"
MANIFEST="${2:?usage: verify_stateful_linux.sh <image> <manifest> [workdir]}"
WORK="${3:-$(mktemp -d)}"
KERNEL_REPLAY=0
if [ "${3:-}" = "--kernel-replay" ]; then
  KERNEL_REPLAY=1
  WORK="$(mktemp -d)"
elif [ "${4:-}" = "--kernel-replay" ]; then
  KERNEL_REPLAY=1
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
  echo "fuzz2: cannot unmount $mp" >&2
  findmnt -T "$mp" || true
  if command -v fuser >/dev/null 2>&1; then
    fuser -vm "$mp" || true
  fi
  umount -l "$mp"
}

check_repair() {
  local output="$1"
  xfs_repair -n "$IMG" >"$output" 2>&1 || {
    cat "$output" >&2
    echo "fuzz2: xfs_repair -n failed" >&2
    return 1
  }
  if grep -qiE 'corruption|fatal|would fix|ALERT|valuable metadata' \
      "$output"; then
    cat "$output" >&2
    echo "fuzz2: xfs_repair -n found corruption" >&2
    return 1
  fi
  tail -1 "$output"
}

if [ "$KERNEL_REPLAY" -eq 0 ]; then
  check_repair "$WORK/xfs_repair_pre.txt"
  python3 "$ROOT/tests/tools/stateful_probe.py" "$IMG" \
    --manifest "$MANIFEST" --quiet
fi

PROFILE="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("profile",""))' "$MANIFEST")"
if [ "$KERNEL_REPLAY" -eq 0 ]; then
  if [ "$PROFILE" = "modern" ]; then
    python3 "$ROOT/tests/tools/rw6e_parent_probe.py" "$IMG" --require-parent \
      --json "$WORK/parent-oracle.json" --quiet
  else
    python3 "$ROOT/tests/tools/rw6e_parent_probe.py" "$IMG" \
      --json "$WORK/parent-oracle.json" --quiet
  fi
fi

# A writable mount lets the kernel replay a dirty log (the crash-image path).
mount -o loop,nouuid "$IMG" "$MNT"
trap 'umount_retry "$MNT" 2>/dev/null || true' EXIT
python3 "$ROOT/tests/tools/stateful_mount_check.py" --mount "$MNT" \
  --manifest "$MANIFEST" --quiet
find "$MNT" -maxdepth 8 -printf '%y %n %s %p -> %l\n' | sort \
  >"$WORK/find.txt"
if command -v xfs_scrub >/dev/null 2>&1; then
  if ! xfs_scrub -n "$MNT" 2>&1 | tee "$WORK/xfs_scrub.txt"; then
    if grep -q 'Kernel metadata scrubbing facility is not available' \
        "$WORK/xfs_scrub.txt"; then
      echo "fuzz2: kernel has no online scrub ioctl; recording a skip" \
        | tee -a "$WORK/xfs_scrub.txt"
    else
      echo "fuzz2: xfs_scrub rejected the image" >&2
      exit 1
    fi
  fi
fi
umount_retry "$MNT"
trap - EXIT

check_repair "$WORK/xfs_repair_post.txt"
if [ "$KERNEL_REPLAY" -eq 1 ]; then
  # The kernel replay is now complete; the independent oracle checks the
  # recovered bytes, not the torn pre-replay state.
  python3 "$ROOT/tests/tools/stateful_probe.py" "$IMG" \
    --manifest "$MANIFEST" --quiet
  if [ "$PROFILE" = "modern" ]; then
    python3 "$ROOT/tests/tools/rw6e_parent_probe.py" "$IMG" --require-parent \
      --json "$WORK/parent-oracle.json" --quiet
  else
    python3 "$ROOT/tests/tools/rw6e_parent_probe.py" "$IMG" \
      --json "$WORK/parent-oracle.json" --quiet
  fi
fi
python3 "$ROOT/tests/tools/log_probe.py" "$IMG" --expect clean \
  >"$WORK/log_probe.txt"
echo "fuzz2: Linux verification passed ($(basename "$IMG"))"
