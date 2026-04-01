# Build And Workflow

## Primary Commands

Use the top-level [`Makefile`](/Users/n1le/Documents/Projects/DFOS/Makefile) for the common flow:

```sh
make headers
make build
make iso
make run
make clean
make check
```

What each target does:

- `headers`: installs libc and kernel headers into `sysroot`
- `build`: builds the freestanding libraries and kernel image, then packs the initrd
- `iso`: creates a BIOS-bootable GRUB ISO for the `i386` QEMU target
- `run`: launches QEMU with the ISO
- `clean`: removes build outputs
- `check`: runs a local Clang syntax pass that does not require the cross toolchain

## Toolchain

The real build still expects:

```sh
export PATH="/path/to/cross/bin:$PATH"
i686-elf-gcc --version
```

The repository uses:

- [`config.sh`](/Users/n1le/Documents/Projects/DFOS/config.sh) for shared environment variables
- [`headers.sh`](/Users/n1le/Documents/Projects/DFOS/headers.sh) for sysroot header install
- [`build.sh`](/Users/n1le/Documents/Projects/DFOS/build.sh) for project builds
- [`iso.sh`](/Users/n1le/Documents/Projects/DFOS/iso.sh) for ISO generation using `grub-mkimage -O i386-pc-eltorito` plus `xorriso`
- [`fetch-grub-i386-pc.sh`](/Users/n1le/Documents/Projects/DFOS/fetch-grub-i386-pc.sh) for downloading the GRUB BIOS platform files into `compile/grub/i386-pc` when they are not installed system-wide
- [`qemu.sh`](/Users/n1le/Documents/Projects/DFOS/qemu.sh) for emulator launch with the ISO attached as the first IDE CD-ROM and `boot order=d`
- [`initrd.sh`](/Users/n1le/Documents/Projects/DFOS/initrd.sh) for tar-based initrd generation

On `arm64` Ubuntu hosts, the build looks for GRUB BIOS platform files in `/usr/lib/grub/i386-pc` first. If they are missing, it downloads `grub-pc-bin` into `compile/grub/i386-pc` using `apt-cache`, `wget`, and `dpkg-deb`. You can override the final lookup path with `GRUB_I386_PC_DIR`.

## Cleanup Changes

The build flow was made more portable by:

- Replacing BSD-incompatible `cp --preserve=timestamps`
- Removing the need for helper scripts to have execute bits set
- Switching the project Makefiles from BSD-style `!=` shell assignment to GNU `make` compatible `$(shell ...)`
- Adding an initrd packaging step to the normal build flow

## Current Limits

- The full kernel image was not link-tested here because `i686-elf-gcc` is not installed in this environment.
- `make check` is only a syntax/integration check using local Clang.
- Running the kernel under QEMU still depends on the expected cross and GRUB tools being present on the host machine.
