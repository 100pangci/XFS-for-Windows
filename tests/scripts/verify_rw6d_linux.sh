#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6D: verify an xattr image with Linux xfs_repair, the independent oracle
# and a read-only kernel mount.
#
# usage: verify_rw6d_linux.sh <image> [--manifest <json>] [--label <text>]
#                               [--require-formats]
#
# The manifest may be the Linux fixture manifest, a Windows mutation
# manifest or a crash manifest.  Attribute values stored in security.* or
# system.* namespaces are verified by the raw oracle; the kernel check only
# compares user.* and trusted.* names (the kernel owns the other encodings).
set -euo pipefail

IMG=${1:?usage: verify_rw6d_linux.sh <image> --manifest <json> [--label <text>]}
shift
MANIFEST=""
LABEL=""
REQUIRE_FORMATS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --manifest) MANIFEST=${2:?}; shift 2 ;;
        --label) LABEL=${2:?}; shift 2 ;;
        --require-formats) REQUIRE_FORMATS=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done
if [ -z "$MANIFEST" ]; then
    echo "--manifest is required" >&2
    exit 1
fi

REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d)
MOUNT="$WORK/mnt"
mkdir -p "$MOUNT"
cleanup() {
    set +e
    mountpoint -q "$MOUNT" && umount -l "$MOUNT"
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ "$(id -u)" != "0" ]; then
    echo "must run as root (loop mount)" >&2
    exit 1
fi
for tool in xfs_repair mount python3; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done

echo "== RW-6D Linux verify ${LABEL:-$IMG} =="
set +e
xfs_repair -n "$IMG" > "$WORK/repair.txt" 2>&1
RC=$?
set -e
cat "$WORK/repair.txt"
if [ "$RC" -ne 0 ] || grep -qiE \
        'corrupt|would fix|bad |error|warn|fatal|alert|dirty|valuable metadata' \
        "$WORK/repair.txt"; then
    echo "xfs_repair rejected $IMG" >&2
    exit 1
fi

INODE_ARGS=()
while read -r ino; do
    [ -n "$ino" ] && INODE_ARGS+=(--inode "$ino")
done < <(python3 - "$MANIFEST" <<'PY'
import json
import sys
manifest = json.load(open(sys.argv[1]))
for item in manifest.get("inodes", []):
    print(item["ino"])
PY
)
python3 "$REPO/tests/tools/rw6d_xattr_probe.py" "$IMG" \
    --manifest "$MANIFEST" "${INODE_ARGS[@]}" \
    --json "$WORK/oracle.json" --quiet

if [ -n "$MANIFEST" ] && [ "$REQUIRE_FORMATS" = "1" ]; then
    python3 - "$WORK/oracle.json" <<'PY'
import json
import sys
inodes = json.load(open(sys.argv[1]))["inodes"]
formats = {item["format"] for item in inodes}
if not {"shortform", "leaf", "node"} <= formats:
    raise SystemExit("missing an attribute representation: %s" %
                     sorted(formats))
if not any(item["format"] == "node" and item["root_level"] >= 2
           for item in inodes):
    raise SystemExit("no multi-level DA node")
if not any(item["aformat"] == 3 for item in inodes):
    raise SystemExit("no attribute BMBT")
if not any(item["remote_blocks"] > 0 for item in inodes):
    raise SystemExit("no remote attribute value")
print("representations OK: %s max-level=%d" %
      (",".join(sorted(formats)),
       max(item["root_level"] for item in inodes)))
PY
fi

mount -o ro,loop "$IMG" "$MOUNT"
if [ -n "$MANIFEST" ]; then
    python3 "$REPO/tests/tools/rw6d_linux_content.py" verify \
        "$MOUNT" "$MANIFEST"
else
    echo "mounted read-only; no manifest supplied"
fi
umount "$MOUNT"
echo "RW-6D Linux verify passed: $IMG"
