#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# FUZZ-2 prepared-image qualification.
#
#   sudo bash tests/scripts/fuzz2_verify_prepared.sh
#
# For every name in NAMES (default "ns xattr data") this expects:
#
#   $SRC/<name>.pre.img       crash image, before project recovery
#   $SRC/<name>.img           project-recovered image
#   $SRC/<name>.manifest.json semantic manifest shared by both
#
# It copies the pre-recovery image to <name>.kernel.img (the kernel replay
# mutates the image), runs the verifier with --kernel-replay, then verifies the
# project-recovered image.  Originals are never modified.  Each phase is logged
# to $SRC/<name>.kernel.log or $SRC/<name>.rec.log.
#
# Environment overrides:
#   SRC    directory holding the images (default /tmp/fuzz2-nonroot)
#   NAMES  space-separated image base names (default "ns xattr data")
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="${SRC:-/tmp/fuzz2-nonroot}"
NAMES="${NAMES:-ns xattr data}"

if [ "$(id -u)" -ne 0 ]; then
  echo "fuzz2: run this script with sudo (it mounts loop devices)" >&2
  exit 2
fi
if [ ! -d "$SRC" ]; then
  echo "fuzz2: missing source directory $SRC" >&2
  exit 1
fi

missing=0
for name in $NAMES; do
  for suffix in pre.img img manifest.json; do
    if [ ! -f "$SRC/$name.$suffix" ]; then
      echo "fuzz2: missing $SRC/$name.$suffix" >&2
      missing=1
    fi
  done
done
if [ "$missing" -ne 0 ]; then
  exit 1
fi

check_not_busy() {
  local image="$1" mnt="$2"
  if losetup -j "$image" 2>/dev/null | grep -q .; then
    echo "fuzz2: $image is still attached to a loop device:" >&2
    losetup -j "$image" >&2 || true
    echo "fuzz2: detach it with: sudo losetup -d <device>" >&2
    exit 1
  fi
  if findmnt -rn -o TARGET 2>/dev/null | grep -Fxq "$mnt"; then
    echo "fuzz2: $mnt is still mounted; unmount it first:" >&2
    echo "fuzz2:   sudo umount -l '$mnt'" >&2
    exit 1
  fi
}

for name in $NAMES; do
  check_not_busy "$SRC/$name.kernel.img" "$SRC/$name.kernel.work/mnt"
  cp "$SRC/$name.pre.img" "$SRC/$name.kernel.img"

  echo "=== $name: kernel replay of the pre-recovery image"
  if ! bash "$ROOT/tests/scripts/verify_stateful_linux.sh" \
      "$SRC/$name.kernel.img" "$SRC/$name.manifest.json" \
      "$SRC/$name.kernel.work" --kernel-replay \
      2>&1 | tee "$SRC/$name.kernel.log"; then
    echo "fuzz2: $name kernel replay FAILED; log: $SRC/$name.kernel.log" >&2
    tail -40 "$SRC/$name.kernel.log" >&2 || true
    exit 1
  fi

  echo "=== $name: project-recovered image"
  check_not_busy "$SRC/$name.img" "$SRC/$name.rec.work/mnt"
  if ! bash "$ROOT/tests/scripts/verify_stateful_linux.sh" \
      "$SRC/$name.img" "$SRC/$name.manifest.json" "$SRC/$name.rec.work" \
      2>&1 | tee "$SRC/$name.rec.log"; then
    echo "fuzz2: $name recovered-image verification FAILED; log: $SRC/$name.rec.log" >&2
    tail -40 "$SRC/$name.rec.log" >&2 || true
    exit 1
  fi
done

echo "fuzz2: all prepared images passed ($SRC)"
