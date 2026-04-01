# DFOS

DFOS is a small educational 32-bit x86 kernel with a higher-half memory layout, Multiboot handoff parsing, an initrd-backed read-only VFS, early paging and PMM support, PIT-driven scheduling, PS/2 keyboard input, and a small in-kernel debugger.

## Project Layout

- [`kernel/`](kernel/) contains the architecture-specific boot code and kernel subsystems.
- [`libc/`](libc/) contains the freestanding support library used by the kernel.
- [`initrd/`](initrd/) contains the files packed into the boot-time ramdisk.
- [`scripts/`](scripts/) contains the build, ISO, GRUB, and QEMU helper scripts.
- [`docs/`](docs/) contains the project documentation index and guides.

## Requirements

The intended toolchain is an `i686-elf` cross compiler plus a few host tools:

- `i686-elf-gcc`
- `i686-elf-ar`
- `grub-file`
- `grub-mkimage`
- `xorriso`
- `wget`
- `qemu-system-i386`

For legacy BIOS ISO generation on `arm64` Ubuntu hosts, the build can fetch GRUB `i386-pc` platform files into `compile/grub/i386-pc` automatically.

## Common Commands

```sh
make headers
make build
make iso
make run
make debug
make boot-layout
make clean
make check
```

Direct script entry points are available under [`scripts/`](scripts/):

```sh
sh ./scripts/headers.sh
sh ./scripts/build.sh
sh ./scripts/iso.sh
sh ./scripts/qemu.sh
sh ./scripts/qemu-debug.sh
```

## Notes

- The kernel currently boots with legacy 32-bit paging enabled from assembly.
- PAE support is detected at runtime and matching page tables are prepared, but the live MMU is not switched into PAE mode yet.
- The multitasking model is kernel-only and uses timer-driven context switching between kernel threads in one shared address space.
- The heap is still an early bootstrap heap backed by the initial paging window.
- Press `F1` at runtime to enter the internal kernel debugger.

## Documentation

- [Documentation Index](docs/README.md)
- [Build And Workflow](docs/build-and-workflow.md)
- [Kernel Architecture](docs/kernel-architecture.md)
- [Kernel Developer Guide](docs/developer-kernel-guide.md)
- [Application Developer Guide](docs/application-developer-guide.md)
- [User Guide](docs/user-guide.md)
