#!/bin/sh
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
export SCRIPT_ROOT=$SCRIPT_DIR
export REPO_ROOT

cd "$REPO_ROOT"
. "$SCRIPT_DIR/config.sh"

mkdir -p "$SYSROOT"

for PROJECT in $SYSTEM_HEADER_PROJECTS; do
  (cd "$PROJECT" && DESTDIR="$SYSROOT" $MAKE install-headers)
done
