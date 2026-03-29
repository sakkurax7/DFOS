# Kernel Architecture

## Overview

The kernel still boots through a Multiboot-compatible 32-bit entry point in [`kernel/arch/i386/boot.S`](/Users/n1le/Documents/Projects/DFOS/kernel/arch/i386/boot.S), but the runtime is now split into clearer subsystems instead of a single `kernel_main`.

Initialization order in [`kernel/kernel/kernel.c`](/Users/n1le/Documents/Projects/DFOS/kernel/kernel/kernel.c):

1. VGA terminal initialization
2. Multiboot validation
3. GDT installation
4. Paging capability setup
5. Physical memory manager initialization
6. Early heap initialization
7. IDT / interrupt setup
8. PIT timer start
9. Scheduler setup, debugger task creation, and worker task creation
10. Interrupt enable and idle loop

## GDT

[`kernel/arch/i386/gdt.c`](/Users/n1le/Documents/Projects/DFOS/kernel/arch/i386/gdt.c) installs a flat segmentation model:

- Null descriptor
- Kernel code segment
- Kernel data segment
- User code segment
- User data segment

[`kernel/arch/i386/gdt_flush.S`](/Users/n1le/Documents/Projects/DFOS/kernel/arch/i386/gdt_flush.S) reloads segment registers and performs the far jump needed to activate the new code segment.

## Interrupts

The interrupt path is split into two pieces:

- [`kernel/arch/i386/interrupts_asm.S`](/Users/n1le/Documents/Projects/DFOS/kernel/arch/i386/interrupts_asm.S): raw ISR stubs, register save/restore, `iret`
- [`kernel/arch/i386/interrupts.c`](/Users/n1le/Documents/Projects/DFOS/kernel/arch/i386/interrupts.c): IDT construction, PIC remap, handler registration, dispatch, and exception panic handling

Implemented vectors:

- `0-31`: CPU exceptions
- `32-47`: PIC IRQ window
- `48`: software yield interrupt used by the scheduler

Timer interrupts are routed through IRQ0 and dispatched to the scheduler. IRQ1 is used by the PS/2 keyboard driver. Page faults are trapped and reported with CR2 and the hardware error code.

## Paging

Bootstrap paging is still enabled in assembly using a higher-half mapping rooted at `0xC0000000`.

[`kernel/kernel/paging.c`](/Users/n1le/Documents/Projects/DFOS/kernel/kernel/paging.c) currently provides:

- Physical/virtual conversion helpers for the higher-half mapping
- Detection of CPU PAE support through CPUID
- Preparation of a matching PAE PDPT/page-directory/page-table layout for the first 4 MiB bootstrap window
- Page-level map/unmap helpers inside the bootstrap-mapped higher-half window

Important limitation:

- The live MMU remains in legacy 32-bit paging mode after boot.
- The kernel prepares PAE tables, but does not yet perform the trampoline-based mode switch required to activate them safely.

## Physical Memory Manager

[`kernel/kernel/pmm.c`](/Users/n1le/Documents/Projects/DFOS/kernel/kernel/pmm.c) parses the Multiboot memory map and builds a bitmap-based frame allocator.

Behavior:

- Starts with all frames reserved
- Marks Multiboot `available` regions free
- Re-reserves low memory, kernel image, Multiboot structures, loaded modules, and the bitmap itself
- Supports single-frame and contiguous-frame allocation/free operations

This is the foundation for future page-table growth and general physical memory ownership tracking.

## Kernel Heap

[`kernel/kernel/heap.c`](/Users/n1le/Documents/Projects/DFOS/kernel/kernel/heap.c) implements an early first-fit allocator:

- Backed by the initial bootstrap-mapped window
- Split/coalesce block management
- `kmalloc`, `kzalloc`, `kfree`

This is intentionally simple and works as an early-kernel memory manager. It is not yet a demand-mapped heap.

## Multitasking

[`kernel/kernel/scheduler.c`](/Users/n1le/Documents/Projects/DFOS/kernel/kernel/scheduler.c) implements kernel thread scheduling:

- Fixed task table
- One kernel stack per task
- Timer-driven round-robin preemption
- Software-interrupt yield path
- Tick-based sleep queue

Tasks share the kernel address space and resume by swapping the saved interrupt frame used by the common ISR return path.

The scheduler also exposes task snapshots for debugging and lets kernel threads yield or exit cleanly.

## Keyboard And Debugger

[`kernel/arch/i386/keyboard.c`](/Users/n1le/Documents/Projects/DFOS/kernel/arch/i386/keyboard.c) implements a PS/2 keyboard driver using set-1 scancodes and a small ring buffer.

[`kernel/kernel/kdebug.c`](/Users/n1le/Documents/Projects/DFOS/kernel/kernel/kdebug.c) provides an internal debugger task entered with `F1`. Current commands:

- `help`
- `tasks`
- `mem`
- `ls`
- `cat <file>`
- `continue`

## Initrd And Filesystem

The kernel now consumes the first Multiboot module as a tar-backed initialization ramdisk.

[`kernel/kernel/vfs.c`](/Users/n1le/Documents/Projects/DFOS/kernel/kernel/vfs.c) exposes a small read-only VFS that indexes files from the initrd archive and supports listing, lookup, and reads.

## Stack Protector

[`kernel/kernel/stack_protector.c`](/Users/n1le/Documents/Projects/DFOS/kernel/kernel/stack_protector.c) defines `__stack_chk_guard` and `__stack_chk_fail`, and the build enables `-fstack-protector-strong`.

## Terminal And Diagnostics

[`kernel/arch/i386/tty.c`](/Users/n1le/Documents/Projects/DFOS/kernel/arch/i386/tty.c) now supports newline handling and scrolling, which makes boot logs and panic messages usable.

[`kernel/kernel/panic.c`](/Users/n1le/Documents/Projects/DFOS/kernel/kernel/panic.c) provides a minimal panic path that disables interrupts, prints a formatted message, and halts.
