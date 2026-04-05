#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

TARGET=${TARGET:-i686-elf}
TOOLCHAIN_ROOT=${DFOS_TOOLCHAIN_ROOT:-"$REPO_ROOT/compile/toolchain"}
TOOLCHAIN_PREFIX=${TOOLCHAIN_PREFIX:-"$TOOLCHAIN_ROOT"}

DOWNLOAD_DIR=${TOOLCHAIN_DOWNLOAD_DIR:-"$TOOLCHAIN_ROOT/downloads"}
SOURCE_DIR=${TOOLCHAIN_SOURCE_DIR:-"$TOOLCHAIN_ROOT/source"}
BUILD_DIR=${TOOLCHAIN_BUILD_DIR:-"$TOOLCHAIN_ROOT/build"}
LOG_DIR=${TOOLCHAIN_LOG_DIR:-"$TOOLCHAIN_ROOT/logs"}
STATE_DIR="$TOOLCHAIN_ROOT/.state"
STEP_STATE_DIR="$STATE_DIR/steps"
BREW_FORMULAE_STATE="$STATE_DIR/brew-formulae-installed-by-script.txt"
ENV_FILE="$TOOLCHAIN_ROOT/env.sh"
FORCE_REBUILD=${FORCE_REBUILD:-0}
GCC_HOST_CC=${GCC_HOST_CC:-}
GCC_HOST_CXX=${GCC_HOST_CXX:-}

BINUTILS_VERSION=${BINUTILS_VERSION:-2.42}
GCC_VERSION=${GCC_VERSION:-13.2.0}
BINUTILS_CONFIGURE_FLAGS=${BINUTILS_CONFIGURE_FLAGS:---with-system-zlib --disable-nls --disable-werror}
GCC_CONFIGURE_FLAGS=${GCC_CONFIGURE_FLAGS:---disable-nls --enable-languages=c --without-headers --with-system-zlib}

BINUTILS_ARCHIVE="binutils-$BINUTILS_VERSION.tar.xz"
GCC_ARCHIVE="gcc-$GCC_VERSION.tar.xz"
BINUTILS_URL=${BINUTILS_URL:-"https://ftp.gnu.org/gnu/binutils/$BINUTILS_ARCHIVE"}
GCC_URL=${GCC_URL:-"https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/$GCC_ARCHIVE"}

BINUTILS_SOURCE_DIR="$SOURCE_DIR/binutils-$BINUTILS_VERSION"
GCC_SOURCE_DIR="$SOURCE_DIR/gcc-$GCC_VERSION"
BINUTILS_BUILD_DIR="$BUILD_DIR/binutils-$BINUTILS_VERSION"
GCC_BUILD_DIR="$BUILD_DIR/gcc-$GCC_VERSION"

GRUB_WORK_ROOT=${GRUB_WORK_ROOT:-"$TOOLCHAIN_ROOT/grub"}
GRUB_INSTALL_ROOT=${GRUB_INSTALL_ROOT:-"$TOOLCHAIN_ROOT/grub-install"}

MAKE=${MAKE:-make}

BREW_FORMULAE="
autoconf
automake
bison
flex
gawk
gcc
gettext
git
gmp
isl
libmpc
libtool
mpfr
pkg-config
qemu
texinfo
wget
xorriso
"

say() {
  printf '%s\n' "$*"
}

fail() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

usage() {
  cat << EOF
Usage: sh ./scripts/macos-toolchain.sh <install|uninstall|status>

Actions:
  install    Install Homebrew dependencies, build the local i686-elf toolchain,
             build a local GRUB i386-pc install, and write compile/toolchain/env.sh.
  uninstall  Remove the local toolchain files and uninstall only the Homebrew
             formulae that this script installed.
  status     Show installation status.

Configuration:
  DFOS_TOOLCHAIN_ROOT   Install root (default: $REPO_ROOT/compile/toolchain)
  TARGET                Cross target triplet (default: i686-elf)
  BINUTILS_VERSION      Binutils version (default: $BINUTILS_VERSION)
  GCC_VERSION           GCC version (default: $GCC_VERSION)
  BINUTILS_CONFIGURE_FLAGS
                       Extra binutils configure flags
                       (default: $BINUTILS_CONFIGURE_FLAGS)
  GCC_CONFIGURE_FLAGS
                       Extra GCC configure flags
                       (default: $GCC_CONFIGURE_FLAGS)
  GCC_HOST_CC
                       Host C compiler used for building GCC itself
  GCC_HOST_CXX
                       Host C++ compiler used for building GCC itself
  FORCE_REBUILD
                       Set to 1 to force all steps to rebuild
EOF
}

command_exists() {
  command -v "$1" >/dev/null 2>&1
}

require_command() {
  if ! command_exists "$1"; then
    fail "required command '$1' was not found in PATH"
  fi
}

require_macos() {
  if [ "$(uname -s)" != "Darwin" ]; then
    fail "this helper is for macOS hosts only"
  fi
}

detect_jobs() {
  jobs=

  if command_exists sysctl; then
    jobs=$(sysctl -n hw.ncpu 2>/dev/null || true)
    if [ -n "$jobs" ]; then
      printf '%s\n' "$jobs"
      return
    fi
  fi

  if command_exists getconf; then
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
    if [ -n "$jobs" ]; then
      printf '%s\n' "$jobs"
      return
    fi
  fi

  echo 1
}

JOBS=${JOBS:-$(detect_jobs)}

mkdir_p() {
  mkdir -p "$1"
}

step_build_signature() {
  cat << EOF
TARGET=$TARGET
TOOLCHAIN_PREFIX=$TOOLCHAIN_PREFIX
BINUTILS_VERSION=$BINUTILS_VERSION
GCC_VERSION=$GCC_VERSION
BINUTILS_CONFIGURE_FLAGS=$BINUTILS_CONFIGURE_FLAGS
GCC_CONFIGURE_FLAGS=$GCC_CONFIGURE_FLAGS
EOF
}

step_marker_path() {
  step_name=$1
  printf '%s\n' "$STEP_STATE_DIR/$step_name.done"
}

step_is_complete() {
  step_name=$1
  shift

  if [ "$FORCE_REBUILD" = "1" ]; then
    return 1
  fi

  marker_file=$(step_marker_path "$step_name")
  if [ ! -f "$marker_file" ]; then
    return 1
  fi

  signature_tmp=$(mktemp)
  step_build_signature > "$signature_tmp"
  if ! cmp -s "$marker_file" "$signature_tmp"; then
    rm -f "$signature_tmp"
    return 1
  fi
  rm -f "$signature_tmp"

  for artifact_path in "$@"; do
    if [ ! -e "$artifact_path" ]; then
      return 1
    fi
  done

  return 0
}

mark_step_complete() {
  step_name=$1
  marker_file=$(step_marker_path "$step_name")
  mkdir_p "$STEP_STATE_DIR"
  step_build_signature > "$marker_file"
}

step_done_message() {
  step_name=$1
  say "Skipping $step_name (already complete)"
}

show_failure_log() {
  log_file=$1
  step_label=$2

  printf 'error: %s failed\n' "$step_label" >&2
  printf 'log file: %s\n' "$log_file" >&2

  if command_exists grep; then
    grep -n -E 'error:|Error [0-9]|undefined reference|fatal:' "$log_file" | tail -n 25 >&2 || true
  fi

  printf '\nLast 120 log lines:\n' >&2
  if command_exists tail; then
    tail -n 120 "$log_file" >&2 || true
  else
    cat "$log_file" >&2 || true
  fi

  fail "$step_label failed"
}

line_in_file() {
  needle=$1
  file=$2

  if [ ! -f "$file" ]; then
    return 1
  fi

  grep -Fx -- "$needle" "$file" >/dev/null 2>&1
}

download_file() {
  url=$1
  output=$2
  tmp_output="$output.tmp"

  if [ -f "$output" ]; then
    return 0
  fi

  say "Downloading $url"

  rm -f "$tmp_output"
  if command_exists curl; then
    curl -fL --retry 3 --retry-delay 2 -o "$tmp_output" "$url"
  elif command_exists wget; then
    wget -O "$tmp_output" "$url"
  else
    fail "need either curl or wget to download source archives"
  fi

  mv "$tmp_output" "$output"
}

extract_tarball() {
  archive=$1
  destination_dir=$2
  expected_dir=$3

  if [ -d "$expected_dir" ]; then
    return 0
  fi

  say "Extracting $(basename "$archive")"
  mkdir_p "$destination_dir"
  tar -xf "$archive" -C "$destination_dir"
}

safe_remove_dir() {
  target_dir=$1

  if [ -z "$target_dir" ] || [ "$target_dir" = "/" ]; then
    fail "refusing to remove unsafe path '$target_dir'"
  fi

  if [ ! -e "$target_dir" ]; then
    return 0
  fi

  case "$target_dir" in
    "$HOME"|"$REPO_ROOT"|"/usr"|"/usr/local"|"/opt"|"/opt/homebrew")
      fail "refusing to remove protected path '$target_dir'"
      ;;
  esac

  rm -rf "$target_dir"
}

ensure_path_within_toolchain_root() {
  path_to_check=$1
  path_label=$2

  case "$path_to_check" in
    "$TOOLCHAIN_ROOT"|"$TOOLCHAIN_ROOT"/*) ;;
    *)
      fail "$path_label must live under $TOOLCHAIN_ROOT for safe install/uninstall (got $path_to_check)"
      ;;
  esac
}

require_brew() {
  if ! command_exists brew; then
    cat >&2 << 'EOF'
error: Homebrew was not found.
Install Homebrew first (https://brew.sh), then rerun this script.
EOF
    exit 1
  fi
}

install_brew_formulae() {
  mkdir_p "$STATE_DIR"
  : > "$BREW_FORMULAE_STATE.tmp"

  for formula in $BREW_FORMULAE; do
    if brew list --formula "$formula" >/dev/null 2>&1; then
      continue
    fi

    say "Installing Homebrew formula: $formula"
    brew install "$formula"

    if ! line_in_file "$formula" "$BREW_FORMULAE_STATE"; then
      printf '%s\n' "$formula" >> "$BREW_FORMULAE_STATE.tmp"
    fi
  done

  if [ -f "$BREW_FORMULAE_STATE" ]; then
    cat "$BREW_FORMULAE_STATE" >> "$BREW_FORMULAE_STATE.tmp"
  fi

  awk '!seen[$0]++' "$BREW_FORMULAE_STATE.tmp" > "$BREW_FORMULAE_STATE"
  rm -f "$BREW_FORMULAE_STATE.tmp"
}

uninstall_brew_formulae() {
  if ! command_exists brew; then
    say "Homebrew is not available; skipping formula uninstall"
    return 0
  fi

  if [ ! -f "$BREW_FORMULAE_STATE" ]; then
    say "No tracked Homebrew installs were found for this script"
    return 0
  fi

  : > "$BREW_FORMULAE_STATE.remaining"

  while IFS= read -r formula; do
    if [ -z "$formula" ]; then
      continue
    fi

    if brew list --formula "$formula" >/dev/null 2>&1; then
      say "Uninstalling Homebrew formula: $formula"
      if ! brew uninstall "$formula"; then
        cat >&2 << EOF
warning: failed to uninstall '$formula'.
You can remove it manually with: brew uninstall $formula
EOF
        printf '%s\n' "$formula" >> "$BREW_FORMULAE_STATE.remaining"
      fi
    fi
  done < "$BREW_FORMULAE_STATE"

  if [ -s "$BREW_FORMULAE_STATE.remaining" ]; then
    mv "$BREW_FORMULAE_STATE.remaining" "$BREW_FORMULAE_STATE"
  else
    rm -f "$BREW_FORMULAE_STATE" "$BREW_FORMULAE_STATE.remaining"
  fi
}

prepend_path() {
  dir=$1
  if [ -z "$dir" ] || [ ! -d "$dir" ]; then
    return
  fi

  case ":$PATH:" in
    *":$dir:"*) ;;
    *) PATH="$dir:$PATH" ;;
  esac
}

setup_brew_build_path() {
  for formula in $BREW_FORMULAE; do
    if brew list --formula "$formula" >/dev/null 2>&1; then
      formula_prefix=$(brew --prefix "$formula")
      prepend_path "$formula_prefix/bin"
      prepend_path "$formula_prefix/sbin"
    fi
  done
  export PATH
}

pick_latest_versioned_binary() {
  bin_dir=$1
  base_name=$2
  best_path=
  best_ver=0

  for candidate in "$bin_dir/$base_name"-*; do
    if [ ! -x "$candidate" ]; then
      continue
    fi

    ver_part=${candidate##*-}
    case "$ver_part" in
      ''|*[!0-9]*)
        continue
        ;;
    esac

    if [ "$ver_part" -gt "$best_ver" ]; then
      best_ver=$ver_part
      best_path=$candidate
    fi
  done

  printf '%s\n' "$best_path"
}

detect_gcc_host_compilers() {
  if [ -n "$GCC_HOST_CC" ] && [ -n "$GCC_HOST_CXX" ]; then
    return 0
  fi

  brew_gcc_prefix=
  if command_exists brew; then
    brew_gcc_prefix=$(brew --prefix gcc 2>/dev/null || true)
  fi

  if [ -n "$brew_gcc_prefix" ] && [ -d "$brew_gcc_prefix/bin" ]; then
    if [ -z "$GCC_HOST_CC" ]; then
      GCC_HOST_CC=$(pick_latest_versioned_binary "$brew_gcc_prefix/bin" gcc)
    fi
    if [ -z "$GCC_HOST_CXX" ]; then
      GCC_HOST_CXX=$(pick_latest_versioned_binary "$brew_gcc_prefix/bin" g++)
    fi
  fi

  if [ -z "$GCC_HOST_CC" ]; then
    GCC_HOST_CC=${CC:-cc}
  fi
  if [ -z "$GCC_HOST_CXX" ]; then
    GCC_HOST_CXX=${CXX:-c++}
  fi

  require_command "$GCC_HOST_CC"
  require_command "$GCC_HOST_CXX"

  if "$GCC_HOST_CXX" --version 2>/dev/null | head -n 1 | grep -qi 'apple clang'; then
    cat >&2 << EOF
error: GCC host C++ compiler resolved to Apple clang ($GCC_HOST_CXX).
Building GCC 13 with Apple's libc++ currently fails on this host due safe-ctype macro conflicts.
Install Homebrew gcc and rerun, or override with:
  GCC_HOST_CC=/path/to/gcc-N GCC_HOST_CXX=/path/to/g++-N
EOF
    exit 1
  fi
}

download_sources() {
  mkdir_p "$DOWNLOAD_DIR" "$SOURCE_DIR"
  download_file "$BINUTILS_URL" "$DOWNLOAD_DIR/$BINUTILS_ARCHIVE"
  download_file "$GCC_URL" "$DOWNLOAD_DIR/$GCC_ARCHIVE"

  extract_tarball "$DOWNLOAD_DIR/$BINUTILS_ARCHIVE" "$SOURCE_DIR" "$BINUTILS_SOURCE_DIR"
  extract_tarball "$DOWNLOAD_DIR/$GCC_ARCHIVE" "$SOURCE_DIR" "$GCC_SOURCE_DIR"
}

build_binutils() {
  say "Building binutils ($BINUTILS_VERSION) for $TARGET"
  mkdir_p "$BINUTILS_BUILD_DIR" "$LOG_DIR"

  if step_is_complete binutils-install \
    "$TOOLCHAIN_PREFIX/bin/$TARGET-ld" \
    "$TOOLCHAIN_PREFIX/bin/$TARGET-ar"; then
    step_done_message "binutils-install"
    return 0
  fi

  binutils_configure_log="$LOG_DIR/binutils-configure.log"
  if step_is_complete binutils-configure "$BINUTILS_BUILD_DIR/Makefile"; then
    step_done_message "binutils-configure"
  else
    say "Configuring binutils (log: $binutils_configure_log)"
    if ! (
      cd "$BINUTILS_BUILD_DIR"
      rm -f config.cache
      "$BINUTILS_SOURCE_DIR/configure" \
        --target="$TARGET" \
        --prefix="$TOOLCHAIN_PREFIX" \
        --with-sysroot \
        $BINUTILS_CONFIGURE_FLAGS
    ) >"$binutils_configure_log" 2>&1; then
      show_failure_log "$binutils_configure_log" "binutils configure"
    fi
    mark_step_complete binutils-configure
  fi

  binutils_build_log="$LOG_DIR/binutils-build.log"
  if step_is_complete binutils-build \
    "$BINUTILS_BUILD_DIR/ld/ld-new" \
    "$BINUTILS_BUILD_DIR/binutils/objdump"; then
    step_done_message "binutils-build"
  else
    say "Compiling binutils (log: $binutils_build_log)"
    if ! "$MAKE" -C "$BINUTILS_BUILD_DIR" -j "$JOBS" >"$binutils_build_log" 2>&1; then
      show_failure_log "$binutils_build_log" "binutils build"
    fi
    mark_step_complete binutils-build
  fi

  binutils_install_log="$LOG_DIR/binutils-install.log"
  if step_is_complete binutils-install \
    "$TOOLCHAIN_PREFIX/bin/$TARGET-ld" \
    "$TOOLCHAIN_PREFIX/bin/$TARGET-ar"; then
    step_done_message "binutils-install"
  else
    say "Installing binutils (log: $binutils_install_log)"
    if ! "$MAKE" -C "$BINUTILS_BUILD_DIR" install >"$binutils_install_log" 2>&1; then
      show_failure_log "$binutils_install_log" "binutils install"
    fi
    mark_step_complete binutils-install
  fi
}

build_gcc() {
  say "Building GCC ($GCC_VERSION) for $TARGET"
  mkdir_p "$GCC_BUILD_DIR" "$LOG_DIR"
  detect_gcc_host_compilers
  say "Using GCC host compilers: CC=$GCC_HOST_CC CXX=$GCC_HOST_CXX"

  gmp_prefix=$(brew --prefix gmp)
  mpfr_prefix=$(brew --prefix mpfr)
  mpc_prefix=$(brew --prefix libmpc)
  isl_prefix=$(brew --prefix isl)
  gcc_install_artifact="$TOOLCHAIN_PREFIX/bin/$TARGET-gcc"
  gcc_install_libgcc_artifact="$TOOLCHAIN_PREFIX/lib/gcc/$TARGET/$GCC_VERSION/libgcc.a"

  if step_is_complete gcc-install-gcc "$gcc_install_artifact" &&
    step_is_complete gcc-install-target-libgcc "$gcc_install_libgcc_artifact"; then
    step_done_message "gcc-install-gcc"
    step_done_message "gcc-install-target-libgcc"
    return 0
  fi

  gcc_configure_log="$LOG_DIR/gcc-configure.log"
  say "Configuring GCC (log: $gcc_configure_log)"
  if ! (
    cd "$GCC_BUILD_DIR"
    rm -f config.cache
    CC="$GCC_HOST_CC" \
    CXX="$GCC_HOST_CXX" \
    "$GCC_SOURCE_DIR/configure" \
      --target="$TARGET" \
      --prefix="$TOOLCHAIN_PREFIX" \
      --with-gmp="$gmp_prefix" \
      --with-mpfr="$mpfr_prefix" \
      --with-mpc="$mpc_prefix" \
      --with-isl="$isl_prefix" \
      $GCC_CONFIGURE_FLAGS
  ) >"$gcc_configure_log" 2>&1; then
    show_failure_log "$gcc_configure_log" "gcc configure"
  fi
  mark_step_complete gcc-configure

  gcc_build_log="$LOG_DIR/gcc-build.log"
  if step_is_complete gcc-all-gcc "$GCC_BUILD_DIR/gcc/xgcc"; then
    step_done_message "gcc-all-gcc"
  else
    say "Compiling GCC stage (log: $gcc_build_log)"
    if ! PATH="$TOOLCHAIN_PREFIX/bin:$PATH" "$MAKE" -C "$GCC_BUILD_DIR" -j "$JOBS" CC="$GCC_HOST_CC" CXX="$GCC_HOST_CXX" all-gcc >"$gcc_build_log" 2>&1; then
      show_failure_log "$gcc_build_log" "gcc all-gcc"
    fi
    mark_step_complete gcc-all-gcc
  fi

  gcc_libgcc_log="$LOG_DIR/gcc-libgcc-build.log"
  if step_is_complete gcc-all-target-libgcc "$GCC_BUILD_DIR/$TARGET/libgcc/libgcc.a"; then
    step_done_message "gcc-all-target-libgcc"
  else
    say "Compiling target libgcc (log: $gcc_libgcc_log)"
    if ! PATH="$TOOLCHAIN_PREFIX/bin:$PATH" "$MAKE" -C "$GCC_BUILD_DIR" -j "$JOBS" CC="$GCC_HOST_CC" CXX="$GCC_HOST_CXX" all-target-libgcc >"$gcc_libgcc_log" 2>&1; then
      show_failure_log "$gcc_libgcc_log" "gcc all-target-libgcc"
    fi
    mark_step_complete gcc-all-target-libgcc
  fi

  gcc_install_log="$LOG_DIR/gcc-install.log"
  if step_is_complete gcc-install-gcc "$gcc_install_artifact"; then
    step_done_message "gcc-install-gcc"
  else
    say "Installing GCC stage (log: $gcc_install_log)"
    if ! PATH="$TOOLCHAIN_PREFIX/bin:$PATH" "$MAKE" -C "$GCC_BUILD_DIR" CC="$GCC_HOST_CC" CXX="$GCC_HOST_CXX" install-gcc >"$gcc_install_log" 2>&1; then
      show_failure_log "$gcc_install_log" "gcc install-gcc"
    fi
    mark_step_complete gcc-install-gcc
  fi

  gcc_libgcc_install_log="$LOG_DIR/gcc-libgcc-install.log"
  if step_is_complete gcc-install-target-libgcc "$gcc_install_libgcc_artifact"; then
    step_done_message "gcc-install-target-libgcc"
  else
    say "Installing target libgcc (log: $gcc_libgcc_install_log)"
    if ! PATH="$TOOLCHAIN_PREFIX/bin:$PATH" "$MAKE" -C "$GCC_BUILD_DIR" CC="$GCC_HOST_CC" CXX="$GCC_HOST_CXX" install-target-libgcc >"$gcc_libgcc_install_log" 2>&1; then
      show_failure_log "$gcc_libgcc_install_log" "gcc install-target-libgcc"
    fi
    mark_step_complete gcc-install-target-libgcc
  fi
}

build_grub() {
  grub_host_awk=awk
  if command_exists gawk; then
    grub_host_awk=gawk
  fi

  say "Building local GRUB i386-pc tools/modules"
  detect_gcc_host_compilers
  say "Using GRUB host compiler: CC=$GCC_HOST_CC"
  say "Using GRUB AWK: $grub_host_awk"
  if step_is_complete grub-install \
    "$GRUB_INSTALL_ROOT/usr/bin/grub-mkimage" \
    "$GRUB_INSTALL_ROOT/usr/lib/grub/i386-pc/cdboot.img"; then
    step_done_message "grub-install"
    return 0
  fi

  PATH="$TOOLCHAIN_PREFIX/bin:$PATH" \
    HOST_CC="$GCC_HOST_CC" \
    HOST_AWK="$grub_host_awk" \
    HOST_CFLAGS="-O2" \
    GRUB_WORK_ROOT="$GRUB_WORK_ROOT" \
    GRUB_INSTALL_ROOT="$GRUB_INSTALL_ROOT" \
    CROSS_TARGET="$TARGET" \
    TARGET_AR="$TOOLCHAIN_PREFIX/bin/$TARGET-ar" \
    TARGET_AS="$TOOLCHAIN_PREFIX/bin/$TARGET-as" \
    TARGET_CC="$TOOLCHAIN_PREFIX/bin/$TARGET-gcc" \
    TARGET_CPP="$TOOLCHAIN_PREFIX/bin/$TARGET-cpp" \
    TARGET_NM="$TOOLCHAIN_PREFIX/bin/$TARGET-nm" \
    TARGET_OBJCOPY="$TOOLCHAIN_PREFIX/bin/$TARGET-objcopy" \
    TARGET_RANLIB="$TOOLCHAIN_PREFIX/bin/$TARGET-ranlib" \
    TARGET_STRIP="$TOOLCHAIN_PREFIX/bin/$TARGET-strip" \
    JOBS="$JOBS" \
    sh "$SCRIPT_DIR/build-grub-i386-pc.sh"

  mark_step_complete grub-install
}

write_env_file() {
  mkdir_p "$TOOLCHAIN_ROOT"

  cat > "$ENV_FILE" << EOF
# Generated by scripts/macos-toolchain.sh
export DFOS_TOOLCHAIN_ROOT="$TOOLCHAIN_ROOT"
export PATH="$TOOLCHAIN_PREFIX/bin:$GRUB_INSTALL_ROOT/usr/bin:\$PATH"
export GRUB_I386_PC_DIR="$GRUB_INSTALL_ROOT/usr/lib/grub/i386-pc"
export LOCAL_GRUB_INSTALL_ROOT="$GRUB_INSTALL_ROOT"
EOF
}

install_toolchain() {
  require_macos
  require_brew
  require_command tar
  require_command "$MAKE"
  ensure_path_within_toolchain_root "$TOOLCHAIN_PREFIX" "TOOLCHAIN_PREFIX"
  ensure_path_within_toolchain_root "$GRUB_WORK_ROOT" "GRUB_WORK_ROOT"
  ensure_path_within_toolchain_root "$GRUB_INSTALL_ROOT" "GRUB_INSTALL_ROOT"

  mkdir_p "$TOOLCHAIN_ROOT"

  install_brew_formulae
  setup_brew_build_path
  download_sources
  build_binutils
  build_gcc
  build_grub
  write_env_file

  cat << EOF
Done.

Local toolchain root: $TOOLCHAIN_ROOT
Cross compiler:       $TOOLCHAIN_PREFIX/bin/$TARGET-gcc
GRUB mkimage:         $GRUB_INSTALL_ROOT/usr/bin/grub-mkimage
GRUB i386-pc dir:     $GRUB_INSTALL_ROOT/usr/lib/grub/i386-pc

For shell sessions outside project scripts:
  source "$ENV_FILE"
EOF
}

uninstall_toolchain() {
  require_macos
  uninstall_brew_formulae
  safe_remove_dir "$TOOLCHAIN_ROOT"
  say "Removed $TOOLCHAIN_ROOT"
}

status_toolchain() {
  require_macos
  printf 'Toolchain root: %s\n' "$TOOLCHAIN_ROOT"

  if [ -x "$TOOLCHAIN_PREFIX/bin/$TARGET-gcc" ]; then
    printf 'Cross compiler: installed (%s)\n' "$TOOLCHAIN_PREFIX/bin/$TARGET-gcc"
  else
    printf 'Cross compiler: not installed\n'
  fi

  if [ -x "$GRUB_INSTALL_ROOT/usr/bin/grub-mkimage" ]; then
    printf 'GRUB mkimage: installed (%s)\n' "$GRUB_INSTALL_ROOT/usr/bin/grub-mkimage"
  else
    printf 'GRUB mkimage: not installed\n'
  fi

  if [ -f "$BREW_FORMULAE_STATE" ]; then
    printf 'Tracked Homebrew installs:\n'
    sed 's/^/  - /' "$BREW_FORMULAE_STATE"
  else
    printf 'Tracked Homebrew installs: none\n'
  fi

  force_rebuild_saved=$FORCE_REBUILD
  FORCE_REBUILD=0
  printf 'Build checkpoints:\n'
  if step_is_complete binutils-install "$TOOLCHAIN_PREFIX/bin/$TARGET-ld" "$TOOLCHAIN_PREFIX/bin/$TARGET-ar"; then
    printf '  - binutils: complete\n'
  else
    printf '  - binutils: pending\n'
  fi
  if step_is_complete gcc-install-gcc "$TOOLCHAIN_PREFIX/bin/$TARGET-gcc" &&
    step_is_complete gcc-install-target-libgcc "$TOOLCHAIN_PREFIX/lib/gcc/$TARGET/$GCC_VERSION/libgcc.a"; then
    printf '  - gcc/libgcc: complete\n'
  else
    printf '  - gcc/libgcc: pending\n'
  fi
  if step_is_complete grub-install "$GRUB_INSTALL_ROOT/usr/bin/grub-mkimage" "$GRUB_INSTALL_ROOT/usr/lib/grub/i386-pc/cdboot.img"; then
    printf '  - grub: complete\n'
  else
    printf '  - grub: pending\n'
  fi
  FORCE_REBUILD=$force_rebuild_saved
}

ACTION=${1:-install}

case "$ACTION" in
  install)
    install_toolchain
    ;;
  uninstall)
    uninstall_toolchain
    ;;
  status)
    status_toolchain
    ;;
  help|-h|--help)
    usage
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac
