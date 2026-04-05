# Build And Workflow

## Primary Commands

Use the top-level [`Makefile`](../Makefile) for the common flow:

```sh
make headers
make build
make iso
make run
make debug
make boot-layout
make clean
make check
make toolchain-macos
make toolchain-macos-uninstall
```

What each target does:

- `headers`: installs libc and kernel headers into `sysroot`
- `build`: builds the freestanding libraries and kernel image, then packs the initrd
- `iso`: creates a BIOS-bootable GRUB ISO for the `i386` QEMU target
- `run`: launches QEMU with the generated ISO
- `debug`: launches QEMU with no auto-reboot and writes `qemu.log`
- `boot-layout`: checks that the bootstrap paging structures are aligned in the built kernel image
- `clean`: removes generated build output
- `check`: runs a local Clang syntax pass that does not require the cross toolchain
- `toolchain-macos`: installs a macOS-local toolchain under `compile/toolchain`
- `toolchain-macos-uninstall`: removes the macOS-local toolchain and tracked dependencies

## Repository Layout

The build-related files are organized like this:

- [`../scripts/`](../scripts/) contains the shell entry points for build, ISO, GRUB, and QEMU tasks
- [`../kernel/include/`](../kernel/include/) contains shared kernel headers and hardware abstraction interfaces
- [`../kernel/kernel/`](../kernel/kernel/) contains architecture-neutral kernel code plus the generic console, input, timer, and IRQ service layers
- [`../kernel/arch/i386/`](../kernel/arch/i386/) contains the current i386 bootstrap code and PC hardware drivers
- [`../libc/`](../libc/) contains the freestanding support library
- [`../initrd/`](../initrd/) contains the files packed into `initrd.tar`
- [`../sysroot/`](../sysroot/) is generated during builds and holds the staged boot artifacts

## Toolchain

The real build still expects:

```sh
export PATH="/path/to/cross/bin:$PATH"
i686-elf-gcc --version
```

On macOS, you can bootstrap the toolchain with:

```sh
sh ./scripts/macos-toolchain.sh install
```

This helper keeps the cross compiler and local GRUB build under `compile/toolchain`, and `scripts/config.sh` will automatically prepend those local binaries to `PATH` when present.

The helper scripts are:

- [`../scripts/config.sh`](../scripts/config.sh) for shared environment variables
- [`../scripts/headers.sh`](../scripts/headers.sh) for sysroot header installation
- [`../scripts/build.sh`](../scripts/build.sh) for project builds
- [`../scripts/initrd.sh`](../scripts/initrd.sh) for tar-based initrd generation
- [`../scripts/iso.sh`](../scripts/iso.sh) for ISO generation using `grub-mkimage -O i386-pc-eltorito` plus `xorriso`
- [`../scripts/fetch-grub-i386-pc.sh`](../scripts/fetch-grub-i386-pc.sh) for downloading the GRUB BIOS platform files into `compile/grub/i386-pc`
- [`../scripts/build-grub-i386-pc.sh`](../scripts/build-grub-i386-pc.sh) for cloning and building GRUB from source into `compile/grub/install`
- [`../scripts/qemu.sh`](../scripts/qemu.sh) for the normal QEMU launcher
- [`../scripts/qemu-debug.sh`](../scripts/qemu-debug.sh) for the debug launcher that keeps the VM from rebooting
- [`../scripts/check-boot-layout.sh`](../scripts/check-boot-layout.sh) for verifying early paging symbol alignment
- [`../scripts/macos-toolchain.sh`](../scripts/macos-toolchain.sh) for macOS-local toolchain install/uninstall under `compile/toolchain`

On `arm64` Ubuntu hosts, the ISO flow looks for GRUB BIOS platform files in `/usr/lib/grub/i386-pc` first. If they are missing, it can download `grub-pc-bin` into `compile/grub/i386-pc` using `apt-cache`, `wget`, and `dpkg-deb`. If you prefer a source build, `scripts/build-grub-i386-pc.sh` installs a local GRUB copy under `compile/grub/install`.

## Build Outputs

The main generated artifacts are:

- `sysroot/boot/dfos.kernel`
- `sysroot/boot/initrd.tar`
- `dfos.iso`
- `qemu.log` when you use the debug launcher
- `compile/grub/` when GRUB assets are fetched or built locally

## Current Limits

- The full kernel image was not link-tested in this environment because the `i686-elf` cross toolchain is not installed here.
- `make check` is only a syntax and integration check using local Clang.
- Running the kernel under QEMU still depends on the expected cross and GRUB tools being present on the host machine.
