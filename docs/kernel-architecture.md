# Kernel Architecture

## Overview

The kernel boots through a Multiboot-compatible 32-bit entry point in [`../kernel/arch/i386/boot.S`](../kernel/arch/i386/boot.S). Early bootstrap data and paging structures are split into dedicated sections so the linker can keep the page directory and page table aligned for the first paging transition.

Initialization order in [`../kernel/kernel/kernel.c`](../kernel/kernel/kernel.c):

1. VGA terminal initialization
2. Stack protector seed
3. Multiboot validation
4. GDT installation
5. IDT installation
6. Paging capability setup
7. Physical memory manager initialization
8. Early heap initialization
9. Keyboard setup
10. Initrd/VFS setup
11. PIT timer start
12. Scheduler setup and bootstrap task registration
13. Debugger task creation and worker task creation
14. Interrupt enable and idle loop

## GDT

[`../kernel/arch/i386/gdt.c`](../kernel/arch/i386/gdt.c) installs a flat segmentation model:

- Null descriptor
- Kernel code segment
- Kernel data segment
- User code segment
- User data segment

[`../kernel/arch/i386/gdt_flush.S`](../kernel/arch/i386/gdt_flush.S) reloads segment registers and performs the far jump needed to activate the new code segment.

## Interrupts

The interrupt path is split into two pieces:

- [`../kernel/arch/i386/interrupts_asm.S`](../kernel/arch/i386/interrupts_asm.S): raw ISR stubs, register save and restore, and `iret`
- [`../kernel/arch/i386/interrupts.c`](../kernel/arch/i386/interrupts.c): IDT construction, PIC remap, handler registration, dispatch, and exception panic handling

Implemented vectors:

- `0-31`: CPU exceptions
- `32-47`: PIC IRQ window
- `48`: software yield interrupt used by the scheduler

Timer interrupts are routed through IRQ0 and dispatched to the scheduler. IRQ1 is used by the PS/2 keyboard driver. Page faults are trapped and reported with CR2 and the hardware error code.

## Paging

Bootstrap paging is enabled in assembly using a higher-half mapping rooted at `0xC0000000`.

[`../kernel/kernel/paging.c`](../kernel/kernel/paging.c) currently provides:

- Physical and virtual conversion helpers for the higher-half mapping
- Detection of CPU PAE support through CPUID
- Preparation of a matching PAE PDPT, page-directory, and page-table layout for the first 4 MiB bootstrap window
- Page-level map and unmap helpers inside the bootstrap-mapped higher-half window

Important limitations:

- The live MMU remains in legacy 32-bit paging mode after boot.
- The bootstrap mapping still covers only the first 4 MiB mirrored at `0xC0000000`.
- The kernel prepares PAE tables, but does not yet perform the trampoline-based mode switch required to activate them safely.

## Physical Memory Manager

[`../kernel/kernel/pmm.c`](../kernel/kernel/pmm.c) parses the Multiboot memory map and builds a bitmap-based frame allocator.

Behavior:

- Starts with all frames reserved
- Marks Multiboot `available` regions free
- Re-reserves low memory, the kernel image, Multiboot structures, loaded modules, and the bitmap itself
- Supports single-frame and contiguous-frame allocation and free operations

This is the foundation for future page-table growth and general physical memory ownership tracking.

## Kernel Heap

[`../kernel/kernel/heap.c`](../kernel/kernel/heap.c) implements an early first-fit allocator:

- Backed by the initial bootstrap-mapped window
- Split and coalesce block management
- `kmalloc`, `kzalloc`, `kfree`

This is intentionally simple and works as an early-kernel allocator. It is not yet a demand-mapped heap.

## Multitasking

[`../kernel/kernel/scheduler.c`](../kernel/kernel/scheduler.c) implements kernel thread scheduling:

- Fixed task table
- One kernel stack per task
- Timer-driven round-robin preemption
- Software-interrupt yield path
- Tick-based sleep queue

Tasks share the kernel address space and resume by swapping the saved interrupt frame used by the common ISR return path.

## Keyboard And Debugger

[`../kernel/arch/i386/keyboard.c`](../kernel/arch/i386/keyboard.c) implements a PS/2 keyboard driver using set-1 scancodes and a small ring buffer.

[`../kernel/kernel/kdebug.c`](../kernel/kernel/kdebug.c) provides an internal debugger task entered with `F1`. Current commands:

- `help`
- `tasks`
- `mem`
- `ls`
- `cat <file>`
- `continue`

## Initrd And Filesystem

The kernel consumes the first Multiboot module as a tar-backed initialization ramdisk.

[`../kernel/kernel/vfs.c`](../kernel/kernel/vfs.c) exposes a small read-only VFS that indexes files from the initrd archive and supports listing, lookup, and reads.

## Stack Protector

[`../kernel/kernel/stack_protector.c`](../kernel/kernel/stack_protector.c) defines `__stack_chk_guard` and `__stack_chk_fail`, and the build enables `-fstack-protector-strong`.

## Terminal And Diagnostics

[`../kernel/arch/i386/tty.c`](../kernel/arch/i386/tty.c) supports newline handling and scrolling so boot logs and panic messages remain readable.

[`../kernel/kernel/panic.c`](../kernel/kernel/panic.c) provides a minimal panic path that disables interrupts, prints a formatted message, and halts.
