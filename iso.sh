#!/bin/sh
set -e
. ./build.sh

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp sysroot/boot/dfos.kernel isodir/boot/dfos.kernel
if [ -f sysroot/boot/initrd.tar ]; then
  cp sysroot/boot/initrd.tar isodir/boot/initrd.tar
fi
cat > isodir/boot/grub/grub.cfg << EOF
menuentry "dfos" {
	multiboot /boot/dfos.kernel
	module /boot/initrd.tar
}
EOF
grub-mkrescue -o dfos.iso isodir
