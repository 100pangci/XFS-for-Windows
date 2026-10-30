#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# FUZZ-2 crash matrix driver.
#
#   sudo EXE=build/tests/stateful_fuzz tests/scripts/fuzz2_crash_matrix.sh
#
# The deterministic plan supplies namespace, xattr and data operations.
# CRASH_DENSITY selects up to that many occurrences of each operation type.
# SKIP_LINUX=1 runs the project recovery checks without kernel qualification.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EXE="${EXE:-$ROOT/build/tests/stateful_fuzz}"
WORK="${WORK:-$(mktemp -d)}"
SEED="${SEED:-0x123456789abcdef}"
STEPS="${STEPS:-80}"
SKIP_LINUX="${SKIP_LINUX:-0}"
SIZE_MB="${SIZE_MB:-300}"
PROFILE="${PROFILE:-modern}"
WORKLOAD="${WORKLOAD:-mixed}"
CRASH_DENSITY="${CRASH_DENSITY:-1}"
KEEP_IMAGES="${KEEP_IMAGES:-0}"
if [[ ! "$CRASH_DENSITY" =~ ^[0-9]+$ ]] || [ "$CRASH_DENSITY" -lt 1 ]; then
  echo "fuzz2: CRASH_DENSITY must be a positive integer" >&2
  exit 2
fi
if [[ ! "$KEEP_IMAGES" =~ ^[01]$ ]]; then
  echo "fuzz2: KEEP_IMAGES must be 0 or 1" >&2
  exit 2
fi
# The ENOSPC workload deliberately has no crash mode (it keeps a live model
# across repeated kNoSpace results); fail before spawning cases.
if [ "$WORKLOAD" = "enospc" ]; then
  echo "fuzz2: the enospc workload does not support crash injection" >&2
  exit 2
fi
if [ "$SKIP_LINUX" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
  echo "fuzz2: kernel verification needs root; run with sudo or set SKIP_LINUX=1" >&2
  exit 2
fi
mkdir -p "$WORK"
WORK_ABS="$(cd "$WORK" && pwd)"

# Backstop for an interrupted or failed kernel verification: the per-image
# script unmounts with a lazy fallback, but a hard failure must not leave a
# mount behind. Also hand failure artifacts back to the invoking sudo user.
cleanup_mounts() {
  local status=$?
  local target
  while IFS= read -r target; do
    [ -n "$target" ] || continue
    case "$target" in
      "$WORK_ABS"/*)
        echo "fuzz2: cleanup: lazy-unmounting leaked mount $target" >&2
        if ! umount -l "$target" 2>/dev/null; then
          echo "fuzz2: cannot release $target; run: sudo umount -l '$target'" >&2
        fi
        ;;
    esac
  done < <(findmnt -rn -o TARGET 2>/dev/null || true)
  if [ "$status" -ne 0 ] && [ -n "${SUDO_UID:-}" ] &&
      [ -n "${SUDO_GID:-}" ]; then
    chown -R "$SUDO_UID:$SUDO_GID" "$WORK" 2>/dev/null || true
  fi
  exit "$status"
}
trap cleanup_mounts EXIT

if [ ! -x "$EXE" ]; then
  echo "fuzz2: missing executable $EXE" >&2
  exit 1
fi

plan="$WORK/plan.txt"
"$EXE" --seed "$SEED" --steps "$STEPS" --image "$WORK/plan.img" \
  --profile "$PROFILE" --workload "$WORKLOAD" \
  --plan-only "$plan" >/dev/null 2>&1

choose_ops() {
  local category="$1"
  awk -v category="$category" '
    $1 ~ /^[0-9]+$/ {
      type = $2
      if ((category == "namespace" &&
           type ~ /^(create|mkdir|rename|hardlink|unlink|rmdir|symlink)$/) ||
          (category == "xattr" &&
           type ~ /^(setxattr|replacexattr|removexattr)$/) ||
          (category == "data" &&
           type ~ /^(write|extend|overwrite|sparse_write|truncate_smaller|truncate_larger|reflink|cow_overwrite|preallocate)$/)) {
        printf "%s\t%s\t%s\n", $1, type, $0
      }
    }' "$plan" | awk -F '\t' -v density="$CRASH_DENSITY" '
      { type = $2; if (count[type]++ < density) print }'
}

namespace_ops="$(choose_ops namespace)"
xattr_ops="$(choose_ops xattr)"
data_ops="$(choose_ops data)"
missing=""
[ -n "$namespace_ops" ] || missing="$missing namespace"
[ -n "$xattr_ops" ] || missing="$missing xattr"
[ -n "$data_ops" ] || missing="$missing data"
if [ -n "$missing" ]; then
  echo "fuzz2: crash matrix needs namespace, xattr and data operations;" \
       "the plan lacks:$missing ($plan)" >&2
  echo "fuzz2: use WORKLOAD=mixed or WORKLOAD=xattr for crash runs;" \
       "dir/alloc plans are for the non-crash workloads." >&2
  exit 1
fi

failures=0
skips=0
run_case() {
  local category="$1" op="$2" type="$3" point="$4"
  local name="$category-$type"
  local base="$WORK/$name-op$op-$point"
  local image="$base.img"
  local trace="$base.trace.txt"
  local manifest="$base.manifest.json"
  rm -f "$image" "$trace" "$manifest" "$manifest.range.bin" \
    "$base.pre.img" "$base.rec.img" "$base.log" "$base.recover.log"

  echo "== $name op=$op point=$point"
  set +e
  "$EXE" --seed "$SEED" --steps "$op" --image "$image" --trace "$trace" \
    --manifest "$manifest" --profile "$PROFILE" --workload "$WORKLOAD" \
    --image-size-mb "$SIZE_MB" --crash-op "$op" --crash-point "$point" \
    --check-every 10 --consistency-every 0 >"$base.log" 2>&1
  local rc=$?
  set -e
  if [ "$rc" -ne 72 ]; then
    if grep -qE "is not applicable to operation|was not reached at operation $op" \
        "$base.log"; then
      echo "== $name op=$op point=$point SKIP (point not reached)"
      skips=$((skips + 1))
      if [ "$KEEP_IMAGES" -eq 0 ]; then
        rm -f "$image" "$trace" "$manifest" "$manifest.range.bin" \
          "$base.log" "$base.recover.log"
      fi
      return 0
    fi
    echo "fuzz2: crash point $point did not terminate (exit $rc)" >&2
    tail -20 "$base.log" >&2
    failures=$((failures + 1))
    return 0
  fi
  cp "$image" "$base.pre.img"

  "$EXE" --seed "$SEED" --steps "$op" --image "$image" --trace "$trace" \
    --manifest "$manifest" --profile "$PROFILE" --workload "$WORKLOAD" \
    --image-size-mb "$SIZE_MB" --crash-op "$op" --crash-point "$point" \
    --recover >"$base.recover.log" 2>&1
  cp "$image" "$base.rec.img"

  if [ "$SKIP_LINUX" -eq 0 ]; then
    bash "$ROOT/tests/scripts/verify_stateful_linux.sh" \
      "$base.pre.img" "$manifest" "$base.pre.work" --kernel-replay
    bash "$ROOT/tests/scripts/verify_stateful_linux.sh" \
      "$base.rec.img" "$manifest" "$base.rec.work"
  fi
  echo "== $name op=$op point=$point OK"
  if [ "$KEEP_IMAGES" -eq 0 ]; then
    rm -f "$base.pre.img" "$base.rec.img" "$image" "$trace" \
      "$manifest" "$manifest.range.bin" "$base.log" "$base.recover.log"
    rm -rf "$base.pre.work" "$base.rec.work"
  fi
}

run_points() {
  local category="$1" op="$2" type="$3" extra_points="$4"
  local common_points="before_log_write mid_log_record after_log_commit before_metadata_write mid_metadata_write after_metadata_write"
  for point in $common_points $extra_points; do
    run_case "$category" "$op" "$type" "$point"
  done
}

while IFS=$'\t' read -r op type _; do
  [ -n "$op" ] || continue
  run_points namespace "$op" "$type" ""
done <<< "$namespace_ops"

while IFS=$'\t' read -r op type details; do
  [ -n "$op" ] || continue
  extra_points=""
  if [[ "$details" =~ length=([0-9]+) ]] &&
      [ "${BASH_REMATCH[1]}" -ge 3072 ]; then
    extra_points="before_data_write after_data_write before_device_flush after_device_flush"
  fi
  run_points xattr "$op" "$type" "$extra_points"
done <<< "$xattr_ops"

while IFS=$'\t' read -r op type _; do
  [ -n "$op" ] || continue
  extra_points=""
  case "$type" in
    write|extend|overwrite|sparse_write)
      extra_points="before_data_write mid_data_write after_data_write"
      ;;
    truncate_smaller)
      extra_points="before_device_flush after_device_flush"
      ;;
    preallocate)
      extra_points="before_data_write mid_data_write after_data_write"
      ;;
    truncate_larger)
      extra_points="before_device_flush after_device_flush"
      ;;
    reflink)
      extra_points="after_bmap_switch"
      ;;
    cow_overwrite)
      extra_points="before_data_write mid_data_write after_data_write cow_staged before_cow_data_write mid_cow_data_write after_cow_data_write before_cow_remap_commit after_cow_remap after_refcount_update after_rmap_update before_old_range_release after_old_range_release before_intent_done after_intent_done"
      ;;
  esac
  run_points data "$op" "$type" "$extra_points"
done <<< "$data_ops"

if [ "$failures" -ne 0 ]; then
  echo "fuzz2: $failures crash cases failed, $skips skipped; artifacts in $WORK" >&2
  exit 1
fi
echo "fuzz2: crash matrix passed ($WORK; skipped=$skips)"
