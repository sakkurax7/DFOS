#!/bin/sh
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
export SCRIPT_ROOT=$SCRIPT_DIR
export REPO_ROOT

cd "$REPO_ROOT"
. "$SCRIPT_DIR/config.sh"

mkdir -p "$SYSROOT$BOOTDIR"
tar -cf "$SYSROOT$BOOTDIR/initrd.tar" -C initrd .
