#!/bin/sh
set -e
. ./config.sh

mkdir -p "$SYSROOT$BOOTDIR"
tar -cf "$SYSROOT$BOOTDIR/initrd.tar" -C initrd .
