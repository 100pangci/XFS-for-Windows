#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# DEV-1 removable-media Linux XFS oracle.
#
#   sudo bash tests/scripts/verify_removable_linux.sh \
#       (--device /dev/sdX|/dev/sdX1 | --image raw-filesystem.img) \
#       --manifest removable-manifest.json [--artifacts DIR]
#
# Device selection is always explicit. Image mode accepts a raw filesystem
# image beginning at byte zero and copies it before any loop mount. In either
# mode, the only xfs_repair operation is guarded and read-only (-n).

set -Eeuo pipefail
IFS=$'\n\t'
export LC_ALL=C

usage() {
    cat >&2 <<'USAGE'
Usage:
  sudo bash verify_removable_linux.sh --device BLOCK_PATH --manifest JSON [--artifacts DIR]
  sudo bash verify_removable_linux.sh --image RAW_FS_IMAGE --manifest JSON [--artifacts DIR]

Exactly one of --device and --image is required. The script never auto-selects a target.
USAGE
}

fail() {
    printf 'FAILED: %s\n' "$*" >&2
    return 1
}

mode=
device=
image=
manifest=
artifacts_arg=
while (($#)); do
    case "$1" in
        --device)
            (($# >= 2)) || { usage; exit 2; }
            [[ -z "$device" && -z "$image" ]] || { usage; fail '--device and --image are mutually exclusive and may appear once'; exit 2; }
            device=$2
            mode=device
            shift 2
            ;;
        --image)
            (($# >= 2)) || { usage; exit 2; }
            [[ -z "$device" && -z "$image" ]] || { usage; fail '--device and --image are mutually exclusive and may appear once'; exit 2; }
            image=$2
            mode=image
            shift 2
            ;;
        --manifest)
            (($# >= 2)) || { usage; exit 2; }
            [[ -z "$manifest" ]] || { usage; fail '--manifest may appear only once'; exit 2; }
            manifest=$2
            shift 2
            ;;
        --artifacts)
            (($# >= 2)) || { usage; exit 2; }
            [[ -z "$artifacts_arg" ]] || { usage; fail '--artifacts may appear only once'; exit 2; }
            artifacts_arg=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            fail "unknown argument: $1"
            exit 2
            ;;
    esac
done

[[ -n "$mode" ]] || { usage; fail 'exactly one of --device or --image is required'; exit 2; }
[[ -n "$manifest" ]] || { usage; fail '--manifest is required'; exit 2; }

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "$script_dir/../.." && pwd -P)"
semantic_probe="$repo_root/tests/tools/removable_semantic_probe.py"
raw_probe="$repo_root/tests/tools/device_image_probe.py"

required_tools=(python3 realpath stat mktemp mkdir rmdir rm cp mount umount mountpoint tee grep date)
required_tools+=(xfs_repair xfs_scrub xfs_logprint)
if [[ "$mode" == device ]]; then
    required_tools+=(lsblk blockdev findmnt)
fi
for tool in "${required_tools[@]}"; do
    command -v "$tool" >/dev/null 2>&1 || {
        fail "missing required tool: $tool"
        exit 1
    }
done

if ((EUID != 0)); then
    fail 'root privileges are required to mount the XFS target; block-device mode also requires root for preflight'
    exit 1
fi
[[ -f "$manifest" ]] || { fail "manifest is not a regular file: $manifest"; exit 1; }
manifest="$(realpath -- "$manifest")"
[[ -f "$semantic_probe" && -f "$raw_probe" ]] || {
    fail 'required removable semantic probe or raw-image probe is missing'
    exit 1
}

# Preflight output goes to a temporary directory outside anything that could
# belong to the target.  No artifact directory or artifact file is created
# until the explicit target identity and the artifact destination have both
# passed their safety checks.
target=
device_identity_verified=false
preflight_dir="$(mktemp -d "${TMPDIR:-/tmp}/xfswin-removable-preflight.XXXXXXXX")"
chmod 700 -- "$preflight_dir"
trap 'rm -rf -- "${preflight_dir:-}"' EXIT

assert_artifacts_not_on_target() {
    local candidate=$1 explicit_device=$2
    local candidate_real tree_device parent mount_point candidate_device mount_device
    candidate_real="$(realpath -m -- "$candidate")"
    tree_device="$explicit_device"
    if [[ "$(lsblk -dno TYPE -- "$explicit_device")" == "part" ]]; then
        parent="$(lsblk -dno PKNAME -- "$explicit_device")"
        [[ -n "$parent" ]] || {
            fail 'cannot resolve the parent disk for the artifact safety check'
            return 1
        }
        tree_device="/dev/$parent"
    fi
    local -a mounts=()
    mapfile -t mounts < <(
        {
            lsblk -lnpo MOUNTPOINTS -- "$tree_device" 2>/dev/null || true
            findmnt -rn -S "$tree_device" -o TARGET 2>/dev/null || true
        } | sed '/^[[:space:]]*$/d' | sort -u
    )
    for mount_point in "${mounts[@]}"; do
        mount_point="$(realpath -- "$mount_point")"
        if [[ "$candidate_real" == "$mount_point" || "$candidate_real" == "$mount_point"/* ]]; then
            fail "artifacts path $candidate lies inside a mounted target filesystem at $mount_point; refusing before creating it"
            return 1
        fi
        local stat_path
        stat_path="$candidate_real"
        [[ -e "$stat_path" ]] || stat_path="$(realpath -m -- "$(dirname -- "$candidate_real")")"
        candidate_device="$(stat -c '%d' -- "$stat_path" 2>/dev/null || true)"
        mount_device="$(stat -c '%d' -- "$mount_point" 2>/dev/null || true)"
        if [[ -n "$candidate_device" && -n "$mount_device" && "$candidate_device" == "$mount_device" ]]; then
            fail "artifacts path $candidate is on the same filesystem as the mounted target at $mount_point; refusing before creating it"
            return 1
        fi
    done
    return 0
}

# 1. Manifest validation reads only local files.
printf '== Validate removable manifest v1 ==\n'
python3 "$semantic_probe" --manifest "$manifest" --validate-only \
    --report-json "$preflight_dir/validation.json" 2>&1 | tee "$preflight_dir/manifest-validation.log"

filesystem_size="$(python3 - "$preflight_dir/validation.json" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    report = json.load(stream)
print(report["filesystem"]["size_bytes"])
PY
)"
filesystem_kind="$(python3 - "$preflight_dir/validation.json" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    report = json.load(stream)
print(report["filesystem"]["target_kind"])
PY
)"

# 2. Explicit device identity and read-only safety checks.  This happens
# before any artifact path is created or written.
if [[ "$mode" == device ]]; then
    device="$(realpath -- "$device")"
    printf '== Verify explicit removable target identity before creating artifacts ==\n'
    python3 "$semantic_probe" --manifest "$manifest" --preflight-device "$device" \
        --report-json "$preflight_dir/identity.json" 2>&1 | tee "$preflight_dir/identity-preflight.log"
    target="$device"
    device_identity_verified=true
fi

# 3. Only now may the artifact directory be created.
if [[ -n "$artifacts_arg" ]]; then
    if [[ "$mode" == device ]]; then
        assert_artifacts_not_on_target "$artifacts_arg" "$device"
    fi
    mkdir -p -- "$artifacts_arg"
    artifacts="$(realpath -- "$artifacts_arg")"
else
    if [[ "$mode" == device ]]; then
        assert_artifacts_not_on_target "$repo_root/build/temp" "$device"
    fi
    mkdir -p -- "$repo_root/build/temp"
    artifacts="$(mktemp -d "$repo_root/build/temp/xfswin-removable-oracle.XXXXXXXX")"
fi

steps_file="$artifacts/steps.txt"
: > "$steps_file"
identity_file="$artifacts/identity-check.json"
cp -- "$preflight_dir/manifest-validation.log" "$artifacts/manifest-validation.log"
if [[ "$mode" == device ]]; then
    cp -- "$preflight_dir/identity.json" "$identity_file"
    cp -- "$preflight_dir/identity-preflight.log" "$artifacts/identity-preflight.log"
else
    cp -- "$preflight_dir/validation.json" "$identity_file"
fi
result_file="$artifacts/result.json"
status_file="$artifacts/oracle-status.txt"
printf 'running\n' > "$status_file"

current_step=preflight
scrub_status=not_run
scrub_exit_code=null
scrub_reason=
mount_dir=
mutation_dir=
work_image=

mark_step() {
    printf '%s=%s\n' "$1" "$2" >> "$steps_file"
}

# The manifest, the explicit device path and the artifact directory were all
# checked before any artifact file was created.  Re-run the read-only identity
# check immediately before each tool that opens the block device again; the
# check refuses a mounted device, so it is called only while unmounted.
recheck_device_identity() {
    [[ "$mode" == device ]] || return 0
    python3 "$semantic_probe" --manifest "$manifest" --preflight-device "$device" \
        --report-json "$identity_file" 2>&1 | tee -a "$artifacts/identity-preflight.log"
}

# Intercept all shell-level xfs_repair calls and refuse anything without -n.
xfs_repair() {
    local arg has_no_modify=0
    for arg in "$@"; do
        if [[ "$arg" == -n ]]; then
            has_no_modify=1
        fi
    done
    if ((has_no_modify == 0)); then
        fail 'internal guard refused xfs_repair invocation without -n'
        return 97
    fi
    command xfs_repair "$@"
}

repair_log_has_failure() {
    python3 - "$1" <<'PY'
import re
import sys

bad = re.compile(
    r"corruption|corrupt|fatal|would\s+(?:fix|clear|modify)|"
    r"dirty\s+log|log\s+(?:needs|requires)\s+replay|"
    r"needs\s+replay|pending\s+replay|replay\s+required|"
    r"valuable metadata changes.*log|log.*not being replayed|"
    r"\berror\s*:", re.IGNORECASE)
with open(sys.argv[1], encoding="utf-8", errors="replace") as stream:
    for line in stream:
        if re.search(r"\bno corruption detected\b", line, re.IGNORECASE):
            continue
        if bad.search(line):
            print(line.rstrip())
            sys.exit(0)
sys.exit(1)
PY
}

write_result() {
    local final_status=$1 exit_code=$2
    python3 - "$result_file" "$final_status" "$exit_code" "$mode" "$target" \
        "$scrub_status" "$scrub_exit_code" "$scrub_reason" "$steps_file" \
        "$identity_file" "$device_identity_verified" "$work_image" "$current_step" <<'PY'
import datetime
import json
import os
import sys

(result_path, status, exit_code, mode, target, scrub_status, scrub_exit,
 scrub_reason, steps_path, identity_path, identity_verified, work_image,
 current_step) = sys.argv[1:]
steps = {}
try:
    with open(steps_path, encoding="utf-8") as stream:
        for line in stream:
            key, sep, value = line.rstrip("\n").partition("=")
            if sep:
                steps[key] = value
except OSError:
    pass
identity = None
if os.path.isfile(identity_path):
    try:
        with open(identity_path, encoding="utf-8") as stream:
            identity = json.load(stream)
    except (OSError, json.JSONDecodeError):
        identity = {"read_error": "identity report could not be decoded"}
logs = [
    "identity-preflight.log", "manifest-validation.log", "image-preflight.log",
    "xfs-repair-before.log", "device-image-probe.log",
    "semantic-probe.log", "linux-mutation.log", "xfs-scrub.log",
    "xfs-logprint.log", "xfs-repair-after.log",
]
result = {
    "schema_version": 1,
    "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "status": status,
    "exit_code": int(exit_code),
    "failed_at_step": current_step if status == "failed" else None,
    "mode": mode,
    "target": target or None,
    "work_image": work_image or None,
    "device_identity_verified": identity_verified == "true",
    "identity_check": identity,
    "steps": steps,
    "scrub": {
        "status": scrub_status,
        "exit_code": None if scrub_exit == "null" else int(scrub_exit),
        "reason": scrub_reason or None,
    },
    "logs": [name for name in logs if os.path.isfile(os.path.join(os.path.dirname(result_path), name))],
}
with open(result_path, "w", encoding="utf-8") as stream:
    json.dump(result, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
}

finish() {
    local rc=$?
    trap - EXIT INT TERM HUP
    if [[ -n "$mount_dir" && -d "$mount_dir" ]] && mountpoint -q -- "$mount_dir"; then
        if [[ -n "$mutation_dir" && -e "$mutation_dir" ]]; then
            if ! rm -rf -- "$mutation_dir"; then
                printf 'FAILED: could not clean the temporary Linux mutation directory\n' >&2
                rc=1
            fi
            mutation_dir=
        fi
        if ! umount -- "$mount_dir"; then
            printf 'FAILED: ordinary umount failed during cleanup for %s\n' "$mount_dir" >&2
            rc=1
        fi
    fi
    if [[ -n "$mount_dir" && -d "$mount_dir" ]] && ! mountpoint -q -- "$mount_dir"; then
        rmdir -- "$mount_dir" 2>/dev/null || rc=1
    fi
    if [[ -n "${preflight_dir:-}" && -d "$preflight_dir" ]]; then
        rm -rf -- "$preflight_dir" || rc=1
    fi
    # A failure before the artifact directory was created has no status file;
    # the terminal output is the evidence for that early refusal.
    if [[ -z "${status_file:-}" || -z "${result_file:-}" || ! -d "${artifacts:-}" ]]; then
        exit "$rc"
    fi
    if ((rc == 0)); then
        printf 'passed\n' > "$status_file"
        if ! write_result passed 0; then
            rc=1
            printf 'failed\n' > "$status_file"
            write_result failed "$rc" || true
        fi
    elif ((rc == 2)) && [[ "$scrub_status" == skipped ]]; then
        printf 'incomplete_scrub_skip\n' > "$status_file"
        write_result incomplete_scrub_skip 2 || true
    else
        printf 'failed\n' > "$status_file"
        write_result failed "$rc" || true
    fi
    exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

# The validation and (device mode) identity preflight ran before the artifact
# directory existed and were copied into it above.
mark_step manifest_validation passed
if [[ "$mode" == device ]]; then
    mark_step device_preflight passed
fi

filesystem_size="$(python3 - "$identity_file" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    report = json.load(stream)
print(report["filesystem"]["size_bytes"])
PY
)"
filesystem_kind="$(python3 - "$identity_file" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    report = json.load(stream)
print(report["filesystem"]["target_kind"])
PY
)"

if [[ "$mode" == image ]]; then
    current_step=image_preflight
    [[ -f "$image" ]] || { fail "--image is not a regular file: $image"; exit 1; }
    image="$(realpath -- "$image")"
    image_size="$(stat -c '%s' -- "$image")"
    [[ "$image_size" == "$filesystem_size" ]] || {
        fail "raw image size mismatch: $image_size != manifest filesystem.size_bytes $filesystem_size"
        exit 1
    }
    work_image="$artifacts/removable-filesystem.raw"
    [[ "$image" != "$work_image" ]] || { fail 'artifacts work image cannot be the source image'; exit 1; }
    cp -- "$image" "$work_image"
    copied_size="$(stat -c '%s' -- "$work_image")"
    [[ "$copied_size" == "$filesystem_size" ]] || {
        fail "work image size changed while copying: $copied_size != $filesystem_size"
        exit 1
    }
    target="$work_image"
    python3 - "$identity_file" "$image" "$filesystem_size" "$filesystem_kind" <<'PY'
import json
import sys
path, source, size_text, target_kind = sys.argv[1:]
with open(path, encoding="utf-8") as stream:
    report = json.load(stream)
report["image_preflight"] = {
    "mode": "image",
    "source_image": source,
    "source_size_bytes": int(size_text),
    "filesystem_target_kind_from_manifest": target_kind,
    "source_device_identity_verified": False,
    "note": "Image mode validates only the raw image size and does not verify live source-device identity.",
}
with open(path, "w", encoding="utf-8") as stream:
    json.dump(report, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
    printf 'CHECK image size equals manifest filesystem.size_bytes: PASS (%s bytes)\n' "$filesystem_size" \
        | tee "$artifacts/image-preflight.log"
    mark_step image_preflight passed
else
    current_step=device_preflight
    recheck_device_identity
    mark_step device_preflight passed
fi

current_step=repair_before
recheck_device_identity
printf '== 1. xfs_repair -n before mount ==\n'
xfs_repair -n "$target" 2>&1 | tee "$artifacts/xfs-repair-before.log"
repair_rc="${PIPESTATUS[0]}"
if ((repair_rc != 0)); then
    fail "xfs_repair -n exited $repair_rc before mount"
    exit 1
fi
if repair_log_has_failure "$artifacts/xfs-repair-before.log"; then
    fail 'xfs_repair -n reported corruption, repair-needed, or dirty-log/replay diagnostics'
    exit 1
fi
mark_step repair_before passed

current_step=raw_probe
printf '== 2. Independent raw image/device probe ==\n'
python3 "$raw_probe" "$target" 2>&1 | tee "$artifacts/device-image-probe.log"
mark_step raw_probe passed

current_step=mount
recheck_device_identity
mount_dir="$(mktemp -d "${TMPDIR:-/tmp}/xfswin-removable-mount.XXXXXXXX")"
if [[ "$mode" == image ]]; then
    mount -t xfs -o rw,loop,nouuid -- "$work_image" "$mount_dir"
else
    mount -t xfs -o rw -- "$target" "$mount_dir"
fi
mark_step mount passed

current_step=semantic_probe
printf '== 3. Mounted removable semantic probe ==\n'
python3 "$semantic_probe" "$mount_dir" --manifest "$manifest" \
    2>&1 | tee "$artifacts/semantic-probe.log"
mark_step semantic_probe passed

current_step=linux_mutation
printf '== 4. Linux-side create/delete/hardlink/rename mutation pass ==\n'
mutation_dir="$(mktemp -d "$mount_dir/.xfswin-linux-mutation.XXXXXXXX")"
printf 'linux oracle mutation\n' > "$mutation_dir/original.txt"
ln -- "$mutation_dir/original.txt" "$mutation_dir/hardlink.txt"
original_inode="$(stat -c '%d:%i' -- "$mutation_dir/original.txt")"
hardlink_inode="$(stat -c '%d:%i' -- "$mutation_dir/hardlink.txt")"
[[ "$original_inode" == "$hardlink_inode" ]] || {
    fail 'Linux mutation hardlink does not share the same device and inode'
    exit 1
}
mv -- "$mutation_dir/original.txt" "$mutation_dir/renamed.txt"
[[ -f "$mutation_dir/renamed.txt" && ! -e "$mutation_dir/original.txt" ]] || {
    fail 'Linux mutation rename did not produce the expected path state'
    exit 1
}
rm -- "$mutation_dir/hardlink.txt" "$mutation_dir/renamed.txt"
rmdir -- "$mutation_dir"
mutation_dir=
printf 'CHECK create/delete/hardlink/rename mutation: PASS\n' | tee "$artifacts/linux-mutation.log"
mark_step linux_mutation passed

current_step=scrub
printf '== 5. xfs_scrub -n while mounted ==\n'
set +e
xfs_scrub -n "$mount_dir" > "$artifacts/xfs-scrub.log" 2>&1
scrub_exit_code=$?
set -e
if ((scrub_exit_code == 0)); then
    scrub_status=passed
    printf '\nresult=passed\nexit_code=0\n' >> "$artifacts/xfs-scrub.log"
    printf 'CHECK xfs_scrub -n: PASS\n'
    mark_step scrub passed
elif grep -Fq 'Kernel metadata scrubbing facility is not available' "$artifacts/xfs-scrub.log"; then
    scrub_status=skipped
    scrub_reason='Kernel metadata scrubbing facility is not available'
    # A scrub skip is useful evidence, but it cannot produce a passing oracle result.
    # Continue the read-only post-mount checks to collect the rest of the evidence.
    printf '\nresult=skipped\nexit_code=%s\nreason=%s\n' "$scrub_exit_code" "$scrub_reason" \
        >> "$artifacts/xfs-scrub.log"
    printf 'CHECK xfs_scrub -n: SKIPPED (%s; exit %s)\n' "$scrub_reason" "$scrub_exit_code"
    mark_step scrub skipped
else
    scrub_status=failed
    scrub_reason='xfs_scrub exited nonzero without the recognized unsupported-kernel diagnostic'
    printf '\nresult=failed\nexit_code=%s\nreason=%s\n' "$scrub_exit_code" "$scrub_reason" \
        >> "$artifacts/xfs-scrub.log"
    fail "xfs_scrub -n failed with exit $scrub_exit_code"
    exit 1
fi

current_step=unmount
printf '== 6. Ordinary clean unmount ==\n'
umount -- "$mount_dir"
mark_step unmount passed
rmdir -- "$mount_dir"
mount_dir=

current_step=logprint
recheck_device_identity
printf '== 7. xfs_logprint ==\n'
xfs_logprint "$target" 2>&1 | tee "$artifacts/xfs-logprint.log"
if grep -Eiq 'bad[[:space:]]+log|corrupt|(^|[^[:alpha:]])error([^[:alpha:]]|$)' "$artifacts/xfs-logprint.log"; then
    fail 'xfs_logprint reported a log error'
    exit 1
fi
mark_step logprint passed

current_step=repair_after
recheck_device_identity
printf '== 8. xfs_repair -n after Linux writable pass ==\n'
xfs_repair -n "$target" 2>&1 | tee "$artifacts/xfs-repair-after.log"
repair_rc="${PIPESTATUS[0]}"
if ((repair_rc != 0)); then
    fail "second xfs_repair -n exited $repair_rc"
    exit 1
fi
if repair_log_has_failure "$artifacts/xfs-repair-after.log"; then
    fail 'second xfs_repair -n reported corruption, repair-needed, or dirty-log/replay diagnostics'
    exit 1
fi
mark_step repair_after passed

current_step=complete
if [[ "$scrub_status" == skipped ]]; then
    printf 'INCOMPLETE: remaining removable Linux XFS checks passed, but scrub was skipped\n' >&2
    exit 2
fi
printf 'OK: removable Linux XFS oracle completed (scrub=%s)\n' "$scrub_status"
