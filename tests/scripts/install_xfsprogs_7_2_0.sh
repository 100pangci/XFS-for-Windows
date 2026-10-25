#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Build the reference xfsprogs from kernel.org and install it under /usr/local.
# The hosted runners' packaged xfsprogs is too old to know `-n parent=1`; the
# RW-6E jobs must use the same tool generation as the on-disk format they are
# qualifying.  This script is intentionally not used by the product build.
set -euo pipefail

VERSION="${XFS_PROGS_VERSION:-7.2.0}"
PREFIX="${XFS_PROGS_PREFIX:-/usr/local}"
JOBS="${JOBS:-$(nproc)}"
WORK="${XFS_PROGS_BUILD_DIR:-$(mktemp -d)}"
URL="https://www.kernel.org/pub/linux/utils/fs/xfs/xfsprogs/xfsprogs-${VERSION}.tar.xz"
TARBALL="$WORK/xfsprogs-${VERSION}.tar.xz"
SRC="$WORK/xfsprogs-${VERSION}"

if [ "$(id -u)" -ne 0 ]; then
  echo "install_xfsprogs: run as root so the build can be installed" >&2
  exit 1
fi

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "install_xfsprogs: missing build tool: $1" >&2
    exit 1
  }
}
for tool in curl tar make gcc; do
  need "$tool"
done

if [ ! -x "$PREFIX/sbin/mkfs.xfs" ]; then
  mkdir -p "$WORK"
  echo "== downloading xfsprogs $VERSION =="
  curl --fail --location --silent --show-error \
    --output "$TARBALL" "$URL"
  tar -C "$WORK" -xf "$TARBALL"
  echo "== configuring xfsprogs $VERSION (prefix=$PREFIX) =="
  (
    cd "$SRC"
    ./configure --prefix="$PREFIX" --disable-gettext
  )
  echo "== building xfsprogs $VERSION =="
  make -C "$SRC" -j"$JOBS"
  echo "== installing xfsprogs $VERSION =="
  make -C "$SRC" install
  ldconfig
else
  echo "== using existing xfsprogs at $PREFIX =="
fi

export PATH="$PREFIX/sbin:$PREFIX/bin:$PATH"
hash -r

if ! mkfs.xfs -V 2>&1 | grep -q "version $VERSION"; then
  echo "install_xfsprogs: $PREFIX/sbin/mkfs.xfs is not version $VERSION" >&2
  mkfs.xfs -V >&2
  exit 1
fi

PROBE="$WORK/parent-probe.img"
truncate -s 64M "$PROBE"
if ! mkfs.xfs -f -q -n parent=1 "$PROBE" >/dev/null 2>&1; then
  echo "install_xfsprogs: built mkfs.xfs still rejects -n parent=1" >&2
  exit 1
fi
rm -f "$PROBE"

echo "rw6e: xfsprogs $VERSION installed at $PREFIX"
mkfs.xfs -V
uname -a
