# Kernel Architecture

## Overview

The kernel boots through a Multiboot-compatible 32-bit entry point in [`../kernel/arch/i386/boot.S`](../kernel/arch/i386/boot.S). Early bootstrap data and paging structures are split into dedicated sections so the linker can keep the page directory and page table aligned for the first paging transition.

The source tree is split into three layers:

- [`../kernel/include/kernel/`](../kernel/include/kernel/) defines the kernel-facing interfaces.
- [`../kernel/kernel/`](../kernel/kernel/) contains architecture-neutral subsystems and the generic hardware service facades.
- [`../kernel/arch/i386/`](../kernel/arch/i386/) contains the current i386 PC bootstrap code and hardware implementations.

Initialization order in [`../kernel/kernel/kernel.c`](../kernel/kernel/kernel.c):

1. Platform module registration and driver selection
2. Active console initialization
3. Stack protector seed
4. Multiboot validation
5. GDT installation
6. IDT installation and IRQ controller initialization
7. Paging capability setup
8. Boot-info handoff normalization
9. Physical memory manager initialization
10. Legacy paging shared-kernel-table finalization (no-op in PAE mode)
11. Early heap initialization
12. Active input driver initialization
13. Initrd/VFS setup
14. Scheduler setup
15. Active timer driver initialization
16. Bootstrap task registration
17. Debugger task creation and worker task creation
18. Interrupt enable and idle loop

## Hardware Abstraction Layer

The kernel now routes hardware-facing work through small generic service interfaces:

- [`../kernel/include/kernel/console.h`](../kernel/include/kernel/console.h) and [`../kernel/kernel/console.c`](../kernel/kernel/console.c) for text output
- [`../kernel/include/kernel/input.h`](../kernel/include/kernel/input.h) and [`../kernel/kernel/input.c`](../kernel/kernel/input.c) for character input and debugger hotkey checks
- [`../kernel/include/kernel/irq.h`](../kernel/include/kernel/irq.h) and [`../kernel/kernel/irq.c`](../kernel/kernel/irq.c) for IRQ controller registration, logical IRQ lines, handler hookup, and acknowledgement
- [`../kernel/include/kernel/timer.h`](../kernel/include/kernel/timer.h) and [`../kernel/kernel/timer.c`](../kernel/kernel/timer.c) for the system tick source
- [`../kernel/include/kernel/cpu.h`](../kernel/include/kernel/cpu.h) for CPU-local helpers such as `hlt` and capability checks
- [`../kernel/include/kernel/module.h`](../kernel/include/kernel/module.h) and [`../kernel/kernel/module.c`](../kernel/kernel/module.c) for static hardware-module registration, probing, and priority-based activation

The current i386 PC platform binds those interfaces in [`../kernel/arch/i386/platform.c`](../kernel/arch/i386/platform.c):

- [`../kernel/arch/i386/vga_console.c`](../kernel/arch/i386/vga_console.c)
- [`../kernel/arch/i386/serial_console.c`](../kernel/arch/i386/serial_console.c)
- [`../kernel/arch/i386/ps2_keyboard.c`](../kernel/arch/i386/ps2_keyboard.c)
- [`../kernel/arch/i386/pic.c`](../kernel/arch/i386/pic.c)
- [`../kernel/arch/i386/pit_timer.c`](../kernel/arch/i386/pit_timer.c)
- [`../kernel/arch/i386/cpu.c`](../kernel/arch/i386/cpu.c)

The console layer can now fan out to more than one backend at a time, which lets early logs appear on both VGA and serial output. Other services still select a single active implementation, chosen by module priority and probe result.

Core kernel code should use the generic interfaces and the module layer, not fixed x86 port numbers or platform-specific helper names.

## Module Registry

[`../kernel/kernel/module.c`](../kernel/kernel/module.c) provides a small static selection mechanism for hardware backends:

- Modules register a name, kind, priority, optional `probe`, and `activate` callback
- Console modules are activated with `module_activate_all`, so multiple output backends can coexist
- Input, timer, and IRQ-controller modules are activated with `module_activate_best`, so the highest-priority working implementation wins
- Activation falls back to lower-priority candidates if a higher-priority module fails

This is the intended path for future APIC-vs-PIC, HPET-vs-PIT, or storage-controller bring-up work.

## GDT

[`../kernel/arch/i386/gdt.c`](../kernel/arch/i386/gdt.c) installs a flat segmentation model plus a hardware task-state segment:

- Null descriptor
- Kernel code segment
- Kernel data segment
- User code segment
- User data segment
- 32-bit available TSS descriptor

[`../kernel/arch/i386/gdt_flush.S`](../kernel/arch/i386/gdt_flush.S) reloads segment registers and performs the far jump needed to activate the new code segment. `gdt_init` then loads the TSS with `ltr`, and the scheduler updates `esp0` on task switches so privilege transitions have a correct ring-0 stack target.

## Interrupts And IRQ Routing

The interrupt path is split into two pieces:

- [`../kernel/arch/i386/interrupts_asm.S`](../kernel/arch/i386/interrupts_asm.S): raw ISR stubs, register save and restore, and `iret`
- [`../kernel/arch/i386/interrupts.c`](../kernel/arch/i386/interrupts.c): IDT construction, handler registration, dispatch, and exception panic handling

Hardware IRQ routing is split again:

- [`../kernel/kernel/irq.c`](../kernel/kernel/irq.c): controller-neutral IRQ registration and vector-to-line translation
- [`../kernel/arch/i386/pic.c`](../kernel/arch/i386/pic.c): current 8259 PIC implementation

Implemented vectors:

- `0-31`: CPU exceptions
- `32-47`: current PIC IRQ window
- `48`: software yield interrupt used by the scheduler

Timer interrupts are routed through logical `IRQ_LINE_TIMER` and dispatched to the scheduler by the active timer driver. Logical `IRQ_LINE_KEYBOARD` is currently backed by the PS/2 keyboard driver. Page faults are trapped and reported with CR2 and the hardware error code.

## CPU Helpers

[`../kernel/include/kernel/cpu.h`](../kernel/include/kernel/cpu.h) exposes the small CPU-facing surface the rest of the kernel uses today. The current implementation in [`../kernel/arch/i386/cpu.c`](../kernel/arch/i386/cpu.c) provides:

- PAE capability detection
- CPU halt in the idle and panic paths

## Paging

Bootstrap paging is enabled in assembly using a higher-half mapping rooted at `0xC0000000`.

[`../kernel/kernel/paging.c`](../kernel/kernel/paging.c) currently provides:

- Detection of CPU PAE support through CPUID
- A safe runtime transition to PAE paging through an identity-mapped assembly trampoline that switches CR0/CR4/CR3 in the required order
- PAE runtime tables with:
  - A low bootstrap PDPT slot used only during the mode switch
  - A higher-half kernel PDPT slot covering `0xC0000000` and above
  - A direct-map aperture for the first 256 MiB of physical memory
  - A dedicated dynamic mapping region (`0xF0000000`-`0xFFBFFFFF`) for on-demand page mapping
- Legacy 32-bit recursive paging fallback when PAE is not available
- Page-level map/unmap helpers across the kernel dynamic mapping range
- `.text` and `.rodata` write protection enforcement by clearing PTE RW bits after paging initialization
- Post-switch identity-map teardown of the lower half so low virtual addresses fault by default
- Process-owned paging-space objects with distinct top-level `CR3` roots
- User/kernel split enforcement (`0x00400000`-`0xBFFFFFFF` user, `0xC0000000+` kernel)
- Explicit user-page map/unmap helpers that target a selected process space
- VMA-tree-backed kernel virtual allocator for dynamic page ranges

Important limitations:

- `paging_phys_to_virt` uses a bounded direct-map aperture (currently first 256 MiB of physical space).
- User-mode execution is not wired yet; the split and mappings are in place for later privilege transitions.

## Physical Memory Manager

[`../kernel/kernel/pmm.c`](../kernel/kernel/pmm.c) consumes memory information through the bootloader-neutral [`../kernel/kernel/bootinfo.c`](../kernel/kernel/bootinfo.c) layer and builds a bitmap-based frame allocator.

Behavior:

- Starts with all frames reserved
- Marks bootloader-reported `available` regions free
- Re-reserves low memory, the kernel image, boot-info structures, loaded modules, and the bitmap itself
- Supports single-frame and contiguous-frame allocation and free operations

This now backs live on-demand page-table growth and general physical memory ownership tracking.

## Kernel Heap

[`../kernel/kernel/heap.c`](../kernel/kernel/heap.c) now implements a slab-based allocator:

- Size-class slab caches for small allocations
- Page-backed path for large allocations
- Reuse of empty slabs with bounded retention per cache
- `kmalloc`, `kzalloc`, `kfree` API remains unchanged for callers

## Multitasking

[`../kernel/kernel/scheduler.c`](../kernel/kernel/scheduler.c) implements kernel thread scheduling:

- Fixed-size thread table with explicit thread lists (free, runnable, sleeping, zombie)
- Per-CPU run queues with strict priority classes
- NUMA-aware task placement policy (preferred node + CPU affinity mask)
- Priority-aware preemption through timer ticks and software-interrupt yield
- Tick-based sleeping and wake-up management
- Extended task-creation API for priority and placement hints

Tasks now own separate paging spaces and the scheduler switches `CR3` during context switches. Execution is still kernel-mode only.

Current implementation notes:

- The scheduling policy is NUMA-aware and SMP-ready, but current i386 bring-up only runs CPU 0.
- The scheduler includes built-in self-tests for thread-list ordering, priority dispatch, and NUMA/affinity CPU selection.

## Input And Debugger

[`../kernel/arch/i386/ps2_keyboard.c`](../kernel/arch/i386/ps2_keyboard.c) implements the current input driver using PS/2 set-1 scancodes and a small ring buffer.

[`../kernel/kernel/kdebug.c`](../kernel/kernel/kdebug.c) polls the generic input interface and provides an internal debugger task entered with `F1`. Current commands:

- `help`
- `tasks`
- `cpus`
- `mem`
- `ls`
- `cat <file>`
- `test [all|<subsystem>|list]`
- `paniclog [clear]`
- `continue`

## Initrd And Filesystem

The kernel consumes the first Multiboot module as a tar-backed initialization ramdisk.

[`../kernel/kernel/vfs.c`](../kernel/kernel/vfs.c) exposes a small read-only VFS that indexes files from the initrd archive and supports listing, lookup, and reads.

## Stack Protector

[`../kernel/kernel/stack_protector.c`](../kernel/kernel/stack_protector.c) defines `__stack_chk_guard` and `__stack_chk_fail`, and the build enables `-fstack-protector-strong`.

## Console And Diagnostics

The current platform exposes two console backends through the generic console layer:

- [`../kernel/arch/i386/vga_console.c`](../kernel/arch/i386/vga_console.c) for VGA text-mode output with scrolling and backspace support
- [`../kernel/arch/i386/serial_console.c`](../kernel/arch/i386/serial_console.c) for polled COM1 output through a 16550-compatible UART

With the default QEMU launchers, serial output is visible in the host terminal because they already use `-serial stdio`.

[`../kernel/kernel/panic.c`](../kernel/kernel/panic.c) now goes through the generic console and CPU interfaces, captures structured crash metadata (tick/task/message), and keeps a persistent in-memory panic history that the debugger can inspect with `paniclog`.
