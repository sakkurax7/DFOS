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

### macOS Helper

For macOS hosts, use the toolchain helper to install everything this repository needs in a project-local location:

```sh
make toolchain-macos
```

It will:

- install missing Homebrew dependencies
- build `i686-elf` binutils and GCC into `compile/toolchain`
- build local GRUB `i386-pc` tools/modules into `compile/toolchain/grub-install`
- write `compile/toolchain/env.sh` for optional manual shell setup

To remove everything the helper installed:

```sh
make toolchain-macos-uninstall
```

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
sh ./scripts/macos-toolchain.sh install
```

## Notes

- The kernel currently boots with legacy 32-bit paging enabled from assembly.
- On PAE-capable CPUs, early boot now performs a safe runtime transition into PAE through an identity-mapped trampoline that toggles CR0/CR4/CR3 in the required order.
- Paging supports per-process address-space objects with process-owned `CR3` roots and a strict user/kernel split (`0x00400000`-`0xBFFFFFFF` user, `0xC0000000+` kernel).
- Kernel dynamic virtual allocations now use a VMA tree allocator in the high-kernel range (`0xF0000000`-`0xFFBFFFFF`).
- Kernel `.text` and `.rodata` pages are write-protected after paging initialization.
- The lower-half bootstrap identity mapping is dropped after the PAE switch, so low virtual addresses fault by default and the user range can be developed cleanly.
- The GDT now includes a TSS descriptor; `ltr` loads it during init and the scheduler updates `esp0` on task switches.
- The scheduler now switches address spaces during task switches, and each kernel task gets its own process page-directory ownership.
- The scheduler uses per-CPU priority runqueues, explicit thread lists, and NUMA-aware CPU selection policy (with affinity + preferred-node hints).
- The heap allocator is now slab-based for small allocations, with page-backed large allocations.
- Boot memory-map parsing now goes through a bootloader-neutral `bootinfo` layer with a Multiboot v1 backend.
- The current i386 platform wires a VGA text console, a COM1 serial console, a PS/2 keyboard, an 8259 PIC, and an 8253/8254 PIT into the generic kernel interfaces through [`kernel/arch/i386/platform.c`](kernel/arch/i386/platform.c).
- Hardware backends are selected through a small static module registry in [`kernel/include/kernel/module.h`](kernel/include/kernel/module.h), which gives future APIC, HPET, or storage-controller work one shared activation pattern.
- Press `F1` at runtime to enter the internal kernel debugger.
- In the debugger, run `test all` (or `test memmap|vma|slab|aspace|sched|sync`) for quick subsystem checks.
- Current open scheduler issue: i386 bring-up still runs only CPU 0, so policy is SMP/NUMA-aware before full multi-core execution support lands.

## Documentation

- [Documentation Index](docs/README.md)
- [Build And Workflow](docs/build-and-workflow.md)
- [Kernel Architecture](docs/kernel-architecture.md)
- [Kernel Developer Guide](docs/developer-kernel-guide.md)
- [Application Developer Guide](docs/application-developer-guide.md)
- [User Guide](docs/user-guide.md)
- [TODO And Roadmap](docs/todo.md)
