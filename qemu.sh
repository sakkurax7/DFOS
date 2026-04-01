#!/bin/sh
set -e
. ./iso.sh

QEMU_ARCH=$(sh ./target-triplet-to-arch.sh "$HOST")

qemu-system-"$QEMU_ARCH" \
  -machine pc \
  -m 256M \
  -boot once=d,menu=on \
  -cdrom dfos.iso \
  -serial stdio \
  "$@"
