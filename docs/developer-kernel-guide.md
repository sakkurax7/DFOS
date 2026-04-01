# Kernel Developer Guide

## Purpose

This guide explains how DFOS is organized today, how to extend it safely, and where the current implementation boundaries are. It is intended for contributors working on the operating system itself.

## System Model

DFOS currently runs as a single-address-space 32-bit x86 kernel:

- Booted by a Multiboot-compatible loader
- Mapped into the higher half at `0xC0000000`
- Running only kernel threads
- Using VGA text mode for output
- Using PIC, PIT, and PS/2-era PC hardware assumptions

There is no user and kernel separation yet. "Applications" currently means:

- New kernel threads created inside the kernel
- Tools and content shipped in the initrd
- Future userland code that will need an ABI once privilege separation exists

## Boot Flow

Entry begins in [`../kernel/arch/i386/boot.S`](../kernel/arch/i386/boot.S).

The bootstrap code:

1. Publishes a Multiboot header for GRUB
2. Preserves the Multiboot handoff registers
3. Clears the early bootstrap paging structures
4. Builds one temporary page directory and page table
5. Identity-maps the early low-memory window long enough to enable paging
6. Mirrors the same mapping into the higher half
7. Jumps to the higher-half virtual address space
8. Calls `kernel_main(multiboot_magic, multiboot_info_addr)`

Important constraints:

- The bootstrap mapping covers only the first 4 MiB of physical memory mirrored at `0xC0000000`.
- Early allocators and tables must fit inside that window unless paging is expanded.
- The page directory and first page table must stay 4 KiB aligned, which is why they live in dedicated linker sections.

## Initialization Order

See [`../kernel/kernel/kernel.c`](../kernel/kernel/kernel.c).

Current initialization sequence:

1. Terminal
2. Stack protector seed
3. Multiboot validation
4. GDT
5. IDT
6. Paging capability setup
7. Physical memory manager
8. Early heap
9. Keyboard
10. VFS and initrd indexing
11. PIT
12. Scheduler
13. Bootstrap current-task registration
14. Debugger task
15. Demo worker tasks
16. Global interrupt enable

That order matters. In particular:

- `pmm_init` depends on bootstrap paging helpers already being usable
- `heap_init` depends on the PMM reserving the bitmap and kernel image
- `kdebug_init` depends on both the scheduler and keyboard

## Memory Subsystems

### Physical Memory Manager

See [`../kernel/kernel/pmm.c`](../kernel/kernel/pmm.c).

Design:

- Bitmap-based frame ownership
- Starts from "all reserved"
- Frees only Multiboot-available regions
- Re-reserves the kernel, bitmap, Multiboot metadata, and modules

Use it when you need:

- A single physical frame: `pmm_alloc_frame`
- A contiguous run of frames: `pmm_alloc_frames`

### Paging

See [`../kernel/kernel/paging.c`](../kernel/kernel/paging.c).

Current paging capabilities:

- Physical and virtual translation for the higher-half window
- Readiness detection and table preparation for PAE
- Page map and unmap inside the existing bootstrap page table
- A monotonic page allocator for virtual addresses inside the early mapped window

Current limits:

- The kernel does not yet grow page tables on demand outside the bootstrap map.
- `paging_alloc_pages` can exhaust the initial virtual window.

### Heap

See [`../kernel/kernel/heap.c`](../kernel/kernel/heap.c).

The heap is intentionally simple:

- First-fit allocator
- Split and coalesce blocks
- No separate slab or object caches
- No backing-store growth yet

For larger future work, prefer building on page allocation rather than stretching the current heap model too far.

## Interrupt Model

See [`../kernel/arch/i386/interrupts.c`](../kernel/arch/i386/interrupts.c) and [`../kernel/arch/i386/interrupts_asm.S`](../kernel/arch/i386/interrupts_asm.S).

Current vector layout:

- `0-31`: CPU exceptions
- `32-47`: PIC IRQ remap window
- `48`: software yield interrupt

How dispatch works:

1. Assembly stubs normalize stack shape
2. `pusha` saves general registers
3. A pointer to the saved frame is passed to `isr_dispatch`
4. C code may return the same frame or a different one
5. Assembly restores registers and returns with `iret`

The scheduler relies on step 4 to resume different tasks.

## Scheduler And Threads

See [`../kernel/kernel/scheduler.c`](../kernel/kernel/scheduler.c).

Current thread model:

- Fixed-size task table
- One kernel stack per task
- Round-robin scheduling
- Tick-based sleeping
- Cooperative yield via `int $48`
- Preemptive time slicing through PIT IRQ0

When creating new kernel services:

- Prefer one dedicated kernel task per long-lived subsystem
- Use `scheduler_sleep` instead of busy loops
- Be aware that shared state needs protection once you add deeper concurrency primitives

Current limits:

- There are no mutexes, semaphores, condition variables, or wait queues yet
- All tasks share one address space
- Zombie tasks are not reaped yet

## Input, Debug, And Files

### Keyboard

See [`../kernel/arch/i386/keyboard.c`](../kernel/arch/i386/keyboard.c).

The keyboard driver currently:

- Assumes a PS/2 keyboard
- Decodes set-1 scancodes
- Buffers translated characters in a small ring buffer
- Reserves `F1` for debugger entry

### Kernel Debugger

See [`../kernel/kernel/kdebug.c`](../kernel/kernel/kdebug.c).

The debugger is a regular kernel thread, not a special stop-the-world monitor. That means:

- It is useful for inspection and basic control
- It does not halt every other thread automatically
- Output from other tasks may interleave unless you later add console locking

### Initrd And VFS

See [`../kernel/kernel/vfs.c`](../kernel/kernel/vfs.c).

The VFS indexes the first Multiboot module as a tar archive and stores pointers directly into that module's memory.

Implications:

- The initrd is read-only
- File contents must stay pinned in RAM
- There is no directory tree object model yet, only indexed file records with path strings

## Coding Conventions For This Repo

Follow the style already present:

- C11 freestanding code
- Short helpers with explicit names
- Targeted comments only around non-obvious control flow or hardware behavior
- Avoid hidden allocation and implicit ownership

When adding comments:

- Explain why or how hardware interaction works
- Avoid narrating obvious assignments or loops

## Recommended Next Steps

If you continue extending DFOS, the next high-value kernel tasks are:

1. Real page-table growth beyond the bootstrap map
2. Locking primitives for shared kernel subsystems
3. Reaping and joining kernel tasks
4. A real vnode and directory-aware VFS
5. User and kernel privilege separation plus a syscall ABI
