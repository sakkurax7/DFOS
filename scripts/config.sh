SYSTEM_HEADER_PROJECTS="libc kernel"
PROJECTS="libc kernel"

if [ -z "${SCRIPT_ROOT:-}" ] || [ -z "${REPO_ROOT:-}" ]; then
  echo "error: SCRIPT_ROOT and REPO_ROOT must be set before sourcing config.sh" >&2
  return 1 2>/dev/null || exit 1
fi

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
