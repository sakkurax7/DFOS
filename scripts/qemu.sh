#!/bin/sh
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
export SCRIPT_ROOT=$SCRIPT_DIR
export REPO_ROOT

cd "$REPO_ROOT"
sh "$SCRIPT_DIR/iso.sh"
. "$SCRIPT_DIR/config.sh"

QEMU_ARCH=$(sh "$SCRIPT_DIR/target-triplet-to-arch.sh" "$HOST")

qemu-system-"$QEMU_ARCH" \
  -machine pc \
  -m 256M \
  -boot once=d,menu=on \
  -cdrom dfos.iso \
  -serial stdio \
  "$@"
