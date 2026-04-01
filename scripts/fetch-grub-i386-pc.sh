#!/bin/sh
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

cd "$REPO_ROOT"

OUTPUT_DIR=${1:-"$REPO_ROOT/compile/grub/i386-pc"}
PACKAGE_NAME=${GRUB_PC_BIN_PACKAGE:-grub-pc-bin}
UBUNTU_MIRROR=${UBUNTU_MIRROR:-https://ports.ubuntu.com/ubuntu-ports}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required tool '$1' was not found in PATH" >&2
    exit 1
  fi
}

require_command apt-cache
require_command dpkg-deb
require_command wget

if [ -f "$OUTPUT_DIR/cdboot.img" ]; then
  exit 0
fi

PACKAGE_PATH=$(apt-cache show "$PACKAGE_NAME" 2>/dev/null | sed -n 's/^Filename: //p' | head -n 1)
if [ -z "$PACKAGE_PATH" ]; then
  cat >&2 << EOF
error: apt metadata for $PACKAGE_NAME was not available.
Run 'apt update' on the Ubuntu build machine, or set GRUB_PC_BIN_PACKAGE and UBUNTU_MIRROR manually.
EOF
  exit 1
fi

TMPDIR=$(mktemp -d)
cleanup() {
  rm -rf "$TMPDIR"
}
trap cleanup EXIT HUP INT TERM

DEB_PATH="$TMPDIR/$PACKAGE_NAME.deb"
EXTRACT_DIR="$TMPDIR/extract"

wget -O "$DEB_PATH" "$UBUNTU_MIRROR/$PACKAGE_PATH"
dpkg-deb -x "$DEB_PATH" "$EXTRACT_DIR"

if [ ! -f "$EXTRACT_DIR/usr/lib/grub/i386-pc/cdboot.img" ]; then
  cat >&2 << EOF
error: downloaded $PACKAGE_NAME did not contain /usr/lib/grub/i386-pc/cdboot.img.
Check that the package and mirror match the Ubuntu arm64 repository for your host.
EOF
  exit 1
fi

mkdir -p "$(dirname "$OUTPUT_DIR")"
rm -rf "$OUTPUT_DIR"
cp -R "$EXTRACT_DIR/usr/lib/grub/i386-pc" "$OUTPUT_DIR"
