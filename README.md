# DFOS

DFOS is a small educational 32-bit x86 kernel that now boots into a structured higher-half environment with:

- Multiboot handoff parsing
- Multiboot module-backed initrd loading
- GDT setup
- IDT and interrupt stubs
- PIC remapping and PIT timer interrupts
- A multithreaded kernel scheduler
- PS/2 keyboard input
- A small internal kernel debugger
- Read-only initrd/VFS support
- Bootstrap paging plus prepared PAE paging tables
- A physical page-frame allocator
- An early kernel heap / memory manager
- Stack-smash protection hooks
- More capable formatted printing
- Cleaner build and run entry points

## Requirements

The intended toolchain is an `i686-elf` cross compiler:

- `i686-elf-gcc`
- `i686-elf-ar`
- `grub-file`
- `grub-mkrescue`
- `qemu-system-i386`

This repository was also adjusted to be friendlier on macOS/BSD userlands where GNU-specific `cp` flags and executable-bit assumptions caused build failures.

## Build

Preferred entry points:

```sh
make headers
make build
make iso
make run
make clean
make check
```

The original shell scripts still exist and are used underneath:

```sh
sh headers.sh
sh build.sh
sh iso.sh
sh qemu.sh
```

The default QEMU launcher attaches `dfos.iso` as the primary IDE CD-ROM and forces `boot order=d` so firmware does not skip past the installation media. The ISO build also validates that the finished image advertises a BIOS El Torito boot entry before treating the build as successful.

## Notes

- The kernel currently boots with legacy 32-bit paging enabled from assembly.
- PAE support is detected at runtime and matching PAE page tables are prepared in C, but the kernel does not yet switch the live MMU into PAE mode during bootstrap.
- The multitasking model is kernel-only and uses timer-driven context switching between kernel threads that share one address space.
- The heap is still an early kernel heap, but paging and PMM helpers now also expose page-level allocation/mapping inside the bootstrap window.
- Keyboard support currently targets a PS/2 controller with set-1 scancodes.
- Press `F1` at runtime to enter the internal kernel debugger.

## Documentation

- [Kernel Architecture](docs/kernel-architecture.md)
- [Build And Workflow](docs/build-and-workflow.md)
- [Kernel Developer Guide](docs/developer-kernel-guide.md)
- [Application Developer Guide](docs/application-developer-guide.md)
- [User Guide](docs/user-guide.md)
