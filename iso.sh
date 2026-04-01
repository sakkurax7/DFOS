#!/bin/sh
set -e
. ./build.sh

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp sysroot/boot/dfos.kernel isodir/boot/dfos.kernel
INITRD_CFG=
if [ -f sysroot/boot/initrd.tar ]; then
  cp sysroot/boot/initrd.tar isodir/boot/initrd.tar
  INITRD_CFG='	module /boot/initrd.tar'
fi
cat > isodir/boot/grub/grub.cfg << EOF
menuentry "dfos" {
	multiboot /boot/dfos.kernel
$INITRD_CFG
}
EOF

if command -v grub-file >/dev/null 2>&1; then
  grub-file --is-x86-multiboot sysroot/boot/dfos.kernel
fi

grub-mkrescue -o dfos.iso isodir

if ! command -v xorriso >/dev/null 2>&1; then
  cat >&2 << 'EOF'
warning: xorriso is not installed, so the finished ISO could not be checked for BIOS El Torito boot support.
If QEMU BIOS says "cannot read the boot disk", install xorriso and re-run make iso to validate the image.
EOF
  exit 0
fi

ELTORITO_REPORT=$(xorriso -indev dfos.iso -report_el_torito plain 2>/dev/null || true)
if ! printf '%s\n' "$ELTORITO_REPORT" | grep -Eq '^El Torito boot img :.*[[:space:]]BIOS[[:space:]]'; then
  cat >&2 << 'EOF'
error: dfos.iso does not appear to advertise a BIOS El Torito boot image.
The kernel may still be valid Multiboot, but this ISO is not bootable by a legacy PC BIOS.
This usually means the local GRUB installation is missing the i386-pc platform files needed by grub-mkrescue.
Reinstall GRUB with BIOS support and rebuild the ISO.
EOF
  printf '%s\n' "$ELTORITO_REPORT" >&2
  exit 1
fi
