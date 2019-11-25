#!/bin/sh
set -e
. ./build.sh

mkdir -p isodir
mkdir -p isodir/boot
mkdir -p isodir/boot/grub

cp sysroot/boot/dfos.kernel isodir/boot/dfos.kernel
cat > isodir/boot/grub/grub.cfg << EOF
menuentry "dfos" {
	multiboot /boot/dfos.kernel
}
EOF
grub-mkrescue -o dfos.iso isodir
