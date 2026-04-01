#!/bin/sh
set -e
. ./iso.sh

QEMU_ARCH=$(sh ./target-triplet-to-arch.sh "$HOST")
QEMU_LOG=${QEMU_LOG:-qemu.log}

qemu-system-"$QEMU_ARCH" \
  -machine pc \
  -m 256M \
  -boot once=d,menu=on \
  -cdrom dfos.iso \
  -serial stdio \
  -no-reboot \
  -no-shutdown \
  -d int,cpu_reset,guest_errors \
  -D "$QEMU_LOG" \
  "$@"
