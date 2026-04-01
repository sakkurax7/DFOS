#!/bin/sh
set -e
. ./build.sh

GRUB_I386_PC_DIR=${GRUB_I386_PC_DIR:-/usr/lib/grub/i386-pc}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: required tool '$1' was not found in PATH" >&2
    exit 1
  fi
}

require_command grub-mkimage
require_command xorriso

if [ ! -d "$GRUB_I386_PC_DIR" ] || [ ! -f "$GRUB_I386_PC_DIR/cdboot.img" ]; then
  cat >&2 << EOF
error: GRUB BIOS platform files were not found at $GRUB_I386_PC_DIR.
This build creates a legacy x86 BIOS ISO and requires the GRUB i386-pc platform files.
On Ubuntu arm64 this is usually provided by the grub-pc-bin package.
You can also override the lookup path by setting GRUB_I386_PC_DIR.
EOF
  exit 1
fi

if command -v grub-file >/dev/null 2>&1; then
  grub-file --is-x86-multiboot sysroot/boot/dfos.kernel
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

grub-mkimage \
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
