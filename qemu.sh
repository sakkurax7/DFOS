#!/bin/sh
set -e
. ./iso.sh

qemu-system-$(sh ./target-triplet-to-arch.sh $HOST) -cdrom dfos.iso
