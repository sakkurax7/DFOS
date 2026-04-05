#!/bin/sh
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

cd "$REPO_ROOT"

sh "$SCRIPT_DIR/build.sh"

DEFAULT_LOCAL_TOOLCHAIN_ROOT=${DFOS_TOOLCHAIN_ROOT:-"$REPO_ROOT/compile/toolchain"}
LOCAL_GRUB_INSTALL_ROOT=${LOCAL_GRUB_INSTALL_ROOT:-"$DEFAULT_LOCAL_TOOLCHAIN_ROOT/grub-install"}
LOCAL_GRUB_BIN_DIR=${LOCAL_GRUB_BIN_DIR:-"$LOCAL_GRUB_INSTALL_ROOT/usr/bin"}
LOCAL_GRUB_BUILD_I386_PC_DIR=${LOCAL_GRUB_BUILD_I386_PC_DIR:-"$LOCAL_GRUB_INSTALL_ROOT/usr/lib/grub/i386-pc"}
LEGACY_LOCAL_GRUB_INSTALL_ROOT=${LEGACY_LOCAL_GRUB_INSTALL_ROOT:-"$REPO_ROOT/compile/grub/install"}
LEGACY_LOCAL_GRUB_BIN_DIR=${LEGACY_LOCAL_GRUB_BIN_DIR:-"$LEGACY_LOCAL_GRUB_INSTALL_ROOT/usr/bin"}
LEGACY_LOCAL_GRUB_BUILD_I386_PC_DIR=${LEGACY_LOCAL_GRUB_BUILD_I386_PC_DIR:-"$LEGACY_LOCAL_GRUB_INSTALL_ROOT/usr/lib/grub/i386-pc"}
LOCAL_GRUB_I386_PC_DIR=${LOCAL_GRUB_I386_PC_DIR:-"$REPO_ROOT/compile/grub/i386-pc"}
GRUB_I386_PC_DIR=${GRUB_I386_PC_DIR:-/usr/lib/grub/i386-pc}
GRUB_PC_BIN_PACKAGE=${GRUB_PC_BIN_PACKAGE:-grub-pc-bin}
GRUB_MKIMAGE=${GRUB_MKIMAGE:-grub-mkimage}
GRUB_FILE=${GRUB_FILE:-grub-file}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required tool '$1' was not found in PATH" >&2
    exit 1
  fi
}

resolve_grub_tool() {
  for tool_candidate in "$@"; do
    if [ -x "$tool_candidate" ]; then
      printf '%s\n' "$tool_candidate"
      return 0
    fi

    if command -v "$tool_candidate" >/dev/null 2>&1; then
      command -v "$tool_candidate"
      return 0
    fi
  done

  return 1
}

GRUB_MKIMAGE=$(resolve_grub_tool \
  "$GRUB_MKIMAGE" \
  "$LOCAL_GRUB_BIN_DIR/grub-mkimage" \
  "$LEGACY_LOCAL_GRUB_BIN_DIR/grub-mkimage") || {
  cat >&2 << EOF
error: GRUB's mkimage tool was not found.
Install GRUB system-wide, run 'sh ./scripts/macos-toolchain.sh install' on macOS, or run
'sh ./scripts/build-grub-i386-pc.sh' to build a local GRUB copy from source.
EOF
  exit 1
}

require_command xorriso

if [ ! -d "$GRUB_I386_PC_DIR" ] || [ ! -f "$GRUB_I386_PC_DIR/cdboot.img" ]; then
  if [ -f "$LOCAL_GRUB_BUILD_I386_PC_DIR/cdboot.img" ]; then
    GRUB_I386_PC_DIR=$LOCAL_GRUB_BUILD_I386_PC_DIR
  elif [ -f "$LEGACY_LOCAL_GRUB_BUILD_I386_PC_DIR/cdboot.img" ]; then
    GRUB_I386_PC_DIR=$LEGACY_LOCAL_GRUB_BUILD_I386_PC_DIR
  else
    if [ ! -f "$LOCAL_GRUB_I386_PC_DIR/cdboot.img" ]; then
      sh "$SCRIPT_DIR/fetch-grub-i386-pc.sh" "$LOCAL_GRUB_I386_PC_DIR"
    fi
    GRUB_I386_PC_DIR=$LOCAL_GRUB_I386_PC_DIR
  fi
fi

if [ ! -d "$GRUB_I386_PC_DIR" ] || [ ! -f "$GRUB_I386_PC_DIR/cdboot.img" ]; then
  cat >&2 << EOF
error: GRUB BIOS platform files were not found at $GRUB_I386_PC_DIR.
This build creates a legacy x86 BIOS ISO and requires the GRUB i386-pc platform files.
The build can fetch them into $LOCAL_GRUB_I386_PC_DIR on Ubuntu arm64, or you can override the lookup path with GRUB_I386_PC_DIR.
EOF
  exit 1
fi

GRUB_FILE=$(resolve_grub_tool \
  "$GRUB_FILE" \
  "$LOCAL_GRUB_BIN_DIR/grub-file" \
  "$LEGACY_LOCAL_GRUB_BIN_DIR/grub-file" || true)
if [ -n "$GRUB_FILE" ]; then
  "$GRUB_FILE" --is-x86-multiboot sysroot/boot/dfos.kernel
fi

TMPDIR=$(mktemp -d)
cleanup() {
  rm -rf "$TMPDIR"
}
trap cleanup EXIT HUP INT TERM

rm -rf isodir
rm -f dfos.iso

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub
mkdir -p isodir/boot/grub/i386-pc

cp sysroot/boot/dfos.kernel isodir/boot/dfos.kernel

INITRD_GRUB_CMD=
if [ -f sysroot/boot/initrd.tar ]; then
  cp sysroot/boot/initrd.tar isodir/boot/initrd.tar
  INITRD_GRUB_CMD='	module /boot/initrd.tar'
fi

cat > isodir/boot/grub/grub.cfg << EOF
search --file --set=root /boot/dfos.kernel

menuentry "dfos" {
	multiboot /boot/dfos.kernel
$INITRD_GRUB_CMD
}
EOF

cat > "$TMPDIR/early-grub.cfg" << 'EOF'
search --file --set=root /boot/dfos.kernel
set prefix=($root)/boot/grub
configfile /boot/grub/grub.cfg
EOF

"$GRUB_MKIMAGE" \
  -O i386-pc-eltorito \
  -d "$GRUB_I386_PC_DIR" \
  -o isodir/boot/grub/i386-pc/eltorito.img \
  -p /boot/grub \
  -c "$TMPDIR/early-grub.cfg" \
  biosdisk iso9660 multiboot normal configfile search search_fs_file

xorriso -as mkisofs \
  -R \
  -J \
  -V DFOS \
  -b boot/grub/i386-pc/eltorito.img \
  -no-emul-boot \
  -boot-load-size 4 \
  -boot-info-table \
  -o dfos.iso \
  isodir

ELTORITO_REPORT=$(xorriso -indev dfos.iso -report_el_torito plain 2>/dev/null || true)
if ! printf '%s\n' "$ELTORITO_REPORT" | grep -Eq '^El Torito boot img :.*[[:space:]]BIOS[[:space:]]'; then
  cat >&2 << 'EOF'
error: dfos.iso does not advertise a BIOS El Torito boot image.
The ISO build completed, but the result is not bootable by a legacy PC BIOS.
Check that grub-mkimage can use the i386-pc platform files and rebuild the ISO.
EOF
  printf '%s\n' "$ELTORITO_REPORT" >&2
  exit 1
fi
