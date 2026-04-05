SYSTEM_HEADER_PROJECTS="libc kernel"
PROJECTS="libc kernel"

if [ -z "${SCRIPT_ROOT:-}" ] || [ -z "${REPO_ROOT:-}" ]; then
  echo "error: SCRIPT_ROOT and REPO_ROOT must be set before sourcing config.sh" >&2
  return 1 2>/dev/null || exit 1
fi

prepend_path_if_dir() {
  if [ -z "$1" ] || [ ! -d "$1" ]; then
    return
  fi

  case ":$PATH:" in
    *":$1:"*) ;;
    *) PATH="$1:$PATH" ;;
  esac
}

LOCAL_TOOLCHAIN_ROOT=${DFOS_TOOLCHAIN_ROOT:-"$REPO_ROOT/compile/toolchain"}
LOCAL_CROSS_BIN_DIR=${LOCAL_CROSS_BIN_DIR:-"$LOCAL_TOOLCHAIN_ROOT/bin"}
LOCAL_GRUB_INSTALL_ROOT=${LOCAL_GRUB_INSTALL_ROOT:-"$LOCAL_TOOLCHAIN_ROOT/grub-install"}
LOCAL_GRUB_BIN_DIR=${LOCAL_GRUB_BIN_DIR:-"$LOCAL_GRUB_INSTALL_ROOT/usr/bin"}
LEGACY_LOCAL_GRUB_BIN_DIR=${LEGACY_LOCAL_GRUB_BIN_DIR:-"$REPO_ROOT/compile/grub/install/usr/bin"}

prepend_path_if_dir "$LOCAL_CROSS_BIN_DIR"
prepend_path_if_dir "$LOCAL_GRUB_BIN_DIR"
prepend_path_if_dir "$LEGACY_LOCAL_GRUB_BIN_DIR"
export PATH

export MAKE=${MAKE:-make}
export HOST=${HOST:-$(sh "$SCRIPT_ROOT/default-host.sh")}

export AR=${HOST}-ar
export AS=${HOST}-as
export CC=${HOST}-gcc

export PREFIX=/usr
export EXEC_PREFIX=$PREFIX
export BOOTDIR=/boot
export LIBDIR=$EXEC_PREFIX/lib
export INCLUDEDIR=$PREFIX/include

export CFLAGS='-O2 -g -fstack-protector-strong'
export CPPFLAGS=''

# Configure the cross-compiler to use the desired system root.
export SYSROOT="$REPO_ROOT/sysroot"
export CC="$CC --sysroot=$SYSROOT"

# Work around that the -elf gcc targets don't have a default system include
# directory when they were configured with --without-headers.
if echo "$HOST" | grep -Eq -- '-elf($|-)'; then
  export CC="$CC -isystem=$INCLUDEDIR"
fi
