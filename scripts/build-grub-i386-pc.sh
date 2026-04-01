#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

GRUB_WORK_ROOT=${GRUB_WORK_ROOT:-"$REPO_ROOT/compile/grub"}
GRUB_SOURCE_DIR=${GRUB_SOURCE_DIR:-"$GRUB_WORK_ROOT/source"}
GRUB_BUILD_DIR=${GRUB_BUILD_DIR:-"$GRUB_WORK_ROOT/build/i386-pc"}
GRUB_INSTALL_ROOT=${GRUB_INSTALL_ROOT:-"$GRUB_WORK_ROOT/install"}

GRUB_GIT_URL=${GRUB_GIT_URL:-https://github.com/rhboot/grub2.git}
GRUB_GIT_REF=${GRUB_GIT_REF:-}

MAKE=${MAKE:-make}
HOST_CC=${HOST_CC:-cc}
HOST_CFLAGS=${HOST_CFLAGS:--O2}
GRUB_CONFIGURE_FLAGS=${GRUB_CONFIGURE_FLAGS:---disable-werror --disable-nls}

CROSS_TARGET=${CROSS_TARGET:-$(sh "$SCRIPT_DIR/default-host.sh")}
TARGET_AR=${TARGET_AR:-${CROSS_TARGET}-ar}
TARGET_AS=${TARGET_AS:-${CROSS_TARGET}-as}
TARGET_CC=${TARGET_CC:-${CROSS_TARGET}-gcc}
TARGET_CPP=${TARGET_CPP:-${CROSS_TARGET}-cpp}
TARGET_NM=${TARGET_NM:-${CROSS_TARGET}-nm}
TARGET_OBJCOPY=${TARGET_OBJCOPY:-${CROSS_TARGET}-objcopy}
TARGET_RANLIB=${TARGET_RANLIB:-${CROSS_TARGET}-ranlib}
TARGET_STRIP=${TARGET_STRIP:-${CROSS_TARGET}-strip}

detect_jobs() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  elif command -v getconf >/dev/null 2>&1; then
    getconf _NPROCESSORS_ONLN
  elif command -v sysctl >/dev/null 2>&1; then
    sysctl -n hw.ncpu
  else
    echo 1
  fi
}

JOBS=${JOBS:-$(detect_jobs)}

command_exists() {
  if [ -x "$1" ]; then
    return 0
  fi

  command -v "$1" >/dev/null 2>&1
}

require_command() {
  if ! command_exists "$1"; then
    echo "error: required tool '$1' was not found in PATH" >&2
    exit 1
  fi
}

update_source_tree() {
  if [ -d "$GRUB_SOURCE_DIR/.git" ]; then
    DIRTY_SOURCE_TREE=$(git -C "$GRUB_SOURCE_DIR" status --short --untracked-files=no)
    if [ -n "$DIRTY_SOURCE_TREE" ] && [ "${GRUB_ALLOW_DIRTY_SOURCE:-0}" != "1" ]; then
      cat >&2 << EOF
error: $GRUB_SOURCE_DIR has local changes.
Commit or stash them first, or set GRUB_ALLOW_DIRTY_SOURCE=1 to skip updating the git checkout.
EOF
      exit 1
    fi

    if [ -n "$DIRTY_SOURCE_TREE" ] && [ "${GRUB_ALLOW_DIRTY_SOURCE:-0}" = "1" ]; then
      echo "warning: skipping git update for dirty source tree $GRUB_SOURCE_DIR" >&2
      return
    fi

    git -C "$GRUB_SOURCE_DIR" fetch --tags origin

    if [ -n "$GRUB_GIT_REF" ]; then
      git -C "$GRUB_SOURCE_DIR" checkout "$GRUB_GIT_REF"
    else
      CURRENT_BRANCH=$(git -C "$GRUB_SOURCE_DIR" symbolic-ref --quiet --short HEAD 2>/dev/null || true)
      if [ -n "$CURRENT_BRANCH" ]; then
        git -C "$GRUB_SOURCE_DIR" pull --ff-only origin "$CURRENT_BRANCH"
      else
        echo "warning: $GRUB_SOURCE_DIR is on a detached HEAD; leaving the current revision checked out" >&2
      fi
    fi

    return
  fi

  mkdir -p "$(dirname "$GRUB_SOURCE_DIR")"
  git clone "$GRUB_GIT_URL" "$GRUB_SOURCE_DIR"

  if [ -n "$GRUB_GIT_REF" ]; then
    git -C "$GRUB_SOURCE_DIR" checkout "$GRUB_GIT_REF"
  fi
}

bootstrap_source_tree() {
  if [ -f "$GRUB_SOURCE_DIR/bootstrap" ]; then
    (cd "$GRUB_SOURCE_DIR" && sh ./bootstrap)
  elif [ -f "$GRUB_SOURCE_DIR/autogen.sh" ]; then
    (cd "$GRUB_SOURCE_DIR" && sh ./autogen.sh)
  else
    echo "error: could not find a GRUB bootstrap script in $GRUB_SOURCE_DIR" >&2
    exit 1
  fi

  if [ ! -x "$GRUB_SOURCE_DIR/configure" ] && [ ! -f "$GRUB_SOURCE_DIR/configure" ]; then
    echo "error: bootstrap did not generate $GRUB_SOURCE_DIR/configure" >&2
    exit 1
  fi
}

configure_build_tree() {
  mkdir -p "$GRUB_BUILD_DIR" "$GRUB_INSTALL_ROOT"

  (
    cd "$GRUB_BUILD_DIR"
    CC="$HOST_CC" \
    CFLAGS="$HOST_CFLAGS" \
    TARGET_AR="$TARGET_AR" \
    TARGET_AS="$TARGET_AS" \
    TARGET_CC="$TARGET_CC" \
    TARGET_CPP="$TARGET_CPP" \
    TARGET_NM="$TARGET_NM" \
    TARGET_OBJCOPY="$TARGET_OBJCOPY" \
    TARGET_RANLIB="$TARGET_RANLIB" \
    TARGET_STRIP="$TARGET_STRIP" \
    "$GRUB_SOURCE_DIR/configure" \
      --prefix=/usr \
      --target=i386 \
      --with-platform=pc \
      $GRUB_CONFIGURE_FLAGS
  )
}

build_and_install() {
  "$MAKE" -C "$GRUB_BUILD_DIR" -j "$JOBS"
  "$MAKE" -C "$GRUB_BUILD_DIR" install DESTDIR="$GRUB_INSTALL_ROOT"
}

require_command git
require_command "$MAKE"
require_command "$HOST_CC"
require_command "$TARGET_AR"
require_command "$TARGET_AS"
require_command "$TARGET_CC"
require_command "$TARGET_CPP"
require_command "$TARGET_NM"
require_command "$TARGET_OBJCOPY"
require_command "$TARGET_RANLIB"
require_command "$TARGET_STRIP"

update_source_tree
bootstrap_source_tree
configure_build_tree
build_and_install

cat << EOF
GRUB source tree:      $GRUB_SOURCE_DIR
GRUB build tree:       $GRUB_BUILD_DIR
GRUB install root:     $GRUB_INSTALL_ROOT
grub-mkimage binary:   $GRUB_INSTALL_ROOT/usr/bin/grub-mkimage
grub-file binary:      $GRUB_INSTALL_ROOT/usr/bin/grub-file
i386-pc program files: $GRUB_INSTALL_ROOT/usr/lib/grub/i386-pc

scripts/iso.sh will automatically use this local install when system GRUB tools are not available.
EOF
