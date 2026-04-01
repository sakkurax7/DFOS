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
- `iso`: creates a GRUB bootable ISO
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
- [`iso.sh`](/Users/n1le/Documents/Projects/DFOS/iso.sh) for ISO generation
- [`qemu.sh`](/Users/n1le/Documents/Projects/DFOS/qemu.sh) for emulator launch with the ISO attached as the first IDE CD-ROM and `boot order=d`
- [`initrd.sh`](/Users/n1le/Documents/Projects/DFOS/initrd.sh) for tar-based initrd generation

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
