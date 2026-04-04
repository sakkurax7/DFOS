# DFOS

DFOS is a small educational 32-bit x86 kernel with a higher-half memory layout, Multiboot handoff parsing, an initrd-backed read-only VFS, early paging and PMM support, timer-driven scheduling, a small in-kernel debugger, and a hardware abstraction layer for console, input, timer, IRQ, CPU, and platform module services.

## Project Layout

- [`kernel/include/`](kernel/include/) contains shared kernel interfaces, including the hardware abstraction headers.
- [`kernel/kernel/`](kernel/kernel/) contains architecture-neutral kernel subsystems and the generic hardware service facades.
- [`kernel/arch/i386/`](kernel/arch/i386/) contains the current i386 PC bootstrap code and hardware driver implementations.
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
- On PAE-capable CPUs, early boot now performs a safe runtime transition into PAE through an identity-mapped trampoline that toggles CR0/CR4/CR3 in the required order.
- Paging supports on-demand growth in a dedicated high-kernel virtual range (`0xF0000000`-`0xFFBFFFFF`).
- Kernel `.text` and `.rodata` pages are write-protected after paging initialization.
- The lower-half bootstrap identity mapping is dropped after the PAE switch, so low virtual addresses fault by default and the user range can be developed cleanly.
- The GDT now includes a TSS descriptor; `ltr` loads it during init and the scheduler updates `esp0` on task switches.
- The multitasking model is kernel-only and uses timer-driven context switching between kernel threads in one shared address space.
- The heap is still an early bootstrap heap backed by the initial paging window.
- The current i386 platform wires a VGA text console, a COM1 serial console, a PS/2 keyboard, an 8259 PIC, and an 8253/8254 PIT into the generic kernel interfaces through [`kernel/arch/i386/platform.c`](kernel/arch/i386/platform.c).
- Hardware backends are selected through a small static module registry in [`kernel/include/kernel/module.h`](kernel/include/kernel/module.h), which gives future APIC, HPET, or storage-controller work one shared activation pattern.
- Press `F1` at runtime to enter the internal kernel debugger.

## Documentation

- [Documentation Index](docs/README.md)
- [Build And Workflow](docs/build-and-workflow.md)
- [Kernel Architecture](docs/kernel-architecture.md)
- [Kernel Developer Guide](docs/developer-kernel-guide.md)
- [Application Developer Guide](docs/application-developer-guide.md)
- [User Guide](docs/user-guide.md)
