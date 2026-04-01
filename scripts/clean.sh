#!/bin/sh
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
export SCRIPT_ROOT=$SCRIPT_DIR
export REPO_ROOT

cd "$REPO_ROOT"
. "$SCRIPT_DIR/config.sh"

for PROJECT in $PROJECTS; do
  (cd "$PROJECT" && $MAKE clean)
done

rm -rf "$SYSROOT"
rm -rf "$REPO_ROOT/isodir"
rm -rf "$REPO_ROOT/dfos.iso"
