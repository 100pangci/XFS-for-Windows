#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# RW-6C ENOSPC Linux verifier.  The Windows pass fills a Linux reflink image
# until even one-block preallocation fails, then requires a 4-block CoW write
# (which retries 2- and 1-block staging runs) to fail without changing data,
# without leaving CoW staging and with valid trees.
#
# usage: verify_rw6c_enospc_linux.sh <image> <enospc.json>
set -euo pipefail

IMG=${1:?usage: verify_rw6c_enospc_linux.sh <image> <enospc.json>}
EVIDENCE=${2:?usage: verify_rw6c_enospc_linux.sh <image> <enospc.json>}
REPO=$(cd "$(dirname "$0")/../.." && pwd)

python3 - "$EVIDENCE" <<'PY'
import json
import sys

d = json.load(open(sys.argv[1]))
required = ("block_size", "free_blocks_before", "free_blocks_after",
            "fill_bytes", "drained_allocations", "failed_index",
            "cow_failure", "cow_sha256", "cow_unchanged", "oracle", "result")
for key in required:
    if key not in d:
        raise SystemExit("evidence missing %s" % key)
if d["result"] != "ok" or not d["cow_unchanged"]:
    raise SystemExit("Windows evidence is not successful")
if int(d["free_blocks_after"]) >= int(d["free_blocks_before"]):
    raise SystemExit("filling did not reduce free space")
if int(d["drained_allocations"]) < 1:
    raise SystemExit("no successful near-ENOSPC allocations")
if not any(w in d["cow_failure"].lower()
           for w in ("no space", "nospace", "enospc", "free space")):
    raise SystemExit("CoW refusal was not a free-space error")
o = d["oracle"]
if o.get("result") != "ok" or int(o.get("cow_records", -1)) != 0:
    raise SystemExit("oracle evidence is not clean: %r" % o)
print("ENOSPC evidence OK: free %d -> %d, CoW refused, no staging" %
      (d["free_blocks_before"], d["free_blocks_after"]))
PY

bash "$REPO/tests/scripts/verify_rw6c_linux.sh" "$IMG" \
    --label "RW-6C ENOSPC image"

WORK=$(mktemp -d)
MOUNT="$WORK/mnt"
mkdir -p "$MOUNT"
cleanup() {
    set +e
    mountpoint -q "$MOUNT" && umount -l "$MOUNT"
    rm -rf "$WORK"
}
trap cleanup EXIT
mount -o ro,loop "$IMG" "$MOUNT"
WANT=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["cow_sha256"])' "$EVIDENCE")
GOT=$(sha256sum "$MOUNT/base/cow.bin" | cut -d' ' -f1)
if [ "$WANT" != "$GOT" ]; then
    echo "CoW target hash $GOT != evidence $WANT" >&2
    exit 1
fi
FREE=$(stat -f -c %f "$MOUNT")
BS=$(stat -f -c %S "$MOUNT")
echo "mounted: free=$FREE blocks of size $BS; CoW target unchanged"
umount "$MOUNT"
trap - EXIT
rm -rf "$WORK"
echo "RW-6C ENOSPC Linux verification passed"
