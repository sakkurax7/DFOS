# Kernel Developer Guide

## Purpose

This guide explains how DFOS is organized today, how to extend it safely, and where the current implementation boundaries are. It is intended for contributors working on the operating system itself.

## System Model

DFOS currently runs as a single-address-space 32-bit x86 kernel:

- Booted by a Multiboot-compatible loader
- Mapped into the higher half at `0xC0000000`
- Running only kernel threads
- Using small generic interfaces for console, input, timer, IRQ, and CPU services
- Backed today by VGA text mode, PIC, PIT, and PS/2-era PC hardware on the i386 platform

There is no user and kernel separation yet. "Applications" currently means:

- New kernel threads created inside the kernel
- Tools and content shipped in the initrd
- Future userland code that will need an ABI once privilege separation exists

## Source Layout

The kernel is organized around a small hardware abstraction boundary:

- [`../kernel/include/kernel/`](../kernel/include/kernel/) contains shared headers and the public subsystem interfaces
- [`../kernel/kernel/`](../kernel/kernel/) contains generic kernel code and the hardware-neutral service facades
- [`../kernel/arch/i386/`](../kernel/arch/i386/) contains the current architecture bootstrap code and concrete hardware drivers

When adding new code, default to `kernel/kernel/` unless the code depends on architecture registers, port I/O, interrupt-controller details, or fixed device memory mappings.

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

- The bootstrap mapping covers the first 4 MiB and exists long enough to perform the PAE transition trampoline.
- On PAE-capable CPUs, `paging_init` installs the larger higher-half PAE map before PMM setup continues.
- In legacy fallback mode, pre-PMM page-table growth uses a fixed bootstrap pool in `.bootstrap_paging`.
- The page directory and first page table must stay 4 KiB aligned, which is why they live in dedicated linker sections.

## Initialization Order

See [`../kernel/kernel/kernel.c`](../kernel/kernel/kernel.c).

Current initialization sequence:

1. Platform module registration and driver selection
2. Console
3. Stack protector seed
4. Multiboot validation
5. GDT
6. IDT and IRQ controller
7. Paging capability setup
8. Physical memory manager
9. Early heap
10. Input
11. VFS and initrd indexing
12. Scheduler
13. Timer
14. Bootstrap current-task registration
15. Debugger task
16. Demo worker tasks
17. Global interrupt enable

That order matters. In particular:

- `pmm_init` depends on bootstrap paging helpers already being usable
- `heap_init` depends on the PMM reserving the bitmap and kernel image
- `input_initialize` depends on the active platform modules already being registered and selected
- `timer_initialize` depends on the IRQ controller being live
- `kdebug_init` depends on both the scheduler and input driver

## Hardware Abstraction Model

The kernel-facing hardware APIs live in:

- [`../kernel/include/kernel/console.h`](../kernel/include/kernel/console.h)
- [`../kernel/include/kernel/input.h`](../kernel/include/kernel/input.h)
- [`../kernel/include/kernel/irq.h`](../kernel/include/kernel/irq.h)
- [`../kernel/include/kernel/timer.h`](../kernel/include/kernel/timer.h)
- [`../kernel/include/kernel/cpu.h`](../kernel/include/kernel/cpu.h)
- [`../kernel/include/kernel/module.h`](../kernel/include/kernel/module.h)

The matching generic dispatch layers live in:

- [`../kernel/kernel/console.c`](../kernel/kernel/console.c)
- [`../kernel/kernel/input.c`](../kernel/kernel/input.c)
- [`../kernel/kernel/irq.c`](../kernel/kernel/irq.c)
- [`../kernel/kernel/timer.c`](../kernel/kernel/timer.c)
- [`../kernel/kernel/module.c`](../kernel/kernel/module.c)

The current i386 platform registers module descriptors in [`../kernel/arch/i386/platform.c`](../kernel/arch/i386/platform.c) and then activates the appropriate implementations.

Current bindings:

- Console: [`../kernel/arch/i386/vga_console.c`](../kernel/arch/i386/vga_console.c) and [`../kernel/arch/i386/serial_console.c`](../kernel/arch/i386/serial_console.c)
- Input: [`../kernel/arch/i386/ps2_keyboard.c`](../kernel/arch/i386/ps2_keyboard.c)
- IRQ controller: [`../kernel/arch/i386/pic.c`](../kernel/arch/i386/pic.c)
- Timer: [`../kernel/arch/i386/pit_timer.c`](../kernel/arch/i386/pit_timer.c)
- CPU helpers: [`../kernel/arch/i386/cpu.c`](../kernel/arch/i386/cpu.c)

Rule of thumb:

- If code can be written in terms of `console_*`, `input_*`, `timer_*`, `irq_*`, or `cpu_*`, it belongs in generic kernel code.
- If code needs `x86_outb`, `x86_inb`, descriptor tables, raw IRQ controller knowledge, or device-specific MMIO addresses, it belongs under the architecture tree.

## Module Selection Pattern

See [`../kernel/include/kernel/module.h`](../kernel/include/kernel/module.h) and [`../kernel/kernel/module.c`](../kernel/kernel/module.c).

Each hardware module declares:

- A `module_kind_t`
- A priority
- An optional `probe` callback
- An `activate` callback that registers the concrete driver with the generic service layer

Use the registry like this:

- For shared output paths such as consoles, register all viable modules and call `module_activate_all`
- For competing implementations such as PIC vs APIC or PIT vs HPET, register all candidates and call `module_activate_best`
- If a higher-priority module fails activation, the registry falls back to the next candidate automatically

That gives you one reusable boot-time selection pattern instead of embedding hardware choice logic in every subsystem.

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

- Safe runtime transition to PAE using an identity-mapped trampoline (`CR0.PG` clear, `CR4.PAE` set, PAE `CR3` load, then `CR0.PG` re-enable)
- PAE runtime layout:
  - `0xC0000000`-`0xCFFFFFFF`: direct-map aperture for first 256 MiB physical
  - `0xF0000000`-`0xFFBFFFFF`: on-demand kernel dynamic mappings
  - Lower-half bootstrap identity map used only for transition, then removed
- Legacy recursive paging fallback when PAE is unavailable
- Physical-to-virtual translation through the direct-map aperture
- Virtual-to-physical translation by page-table walk
- Page map/unmap across the kernel dynamic range
- Kernel section hardening: `.text` and `.rodata` pages are marked read-only
- A monotonic page allocator in the dedicated high virtual dynamic range

Current limits:

- `paging_phys_to_virt` currently supports a bounded direct-map aperture (first 256 MiB physical).
- Per-process user address spaces are not implemented yet; the kernel still runs in a shared address space model.

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
- `32-47`: current PIC IRQ remap window
- `48`: software yield interrupt

How dispatch works:

1. Assembly stubs normalize stack shape
2. `pusha` saves general registers
3. A pointer to the saved frame is passed to `isr_dispatch`
4. C code may return the same frame or a different one
5. Assembly restores registers and returns with `iret`

The scheduler relies on step 4 to resume different tasks.

IRQ-aware drivers should use the generic IRQ layer in [`../kernel/kernel/irq.c`](../kernel/kernel/irq.c) rather than hard-coding vector numbers in generic code. Today there are logical lines for the system timer and keyboard, and the active controller maps those to hardware vectors.

## Scheduler And Threads

See [`../kernel/kernel/scheduler.c`](../kernel/kernel/scheduler.c).

Current thread model:

- Fixed-size task table
- One kernel stack per task
- Scheduler-maintained TSS `esp0` updates on task switches
- Round-robin scheduling
- Tick-based sleeping
- Cooperative yield via `int $48`
- Preemptive time slicing through the active timer driver

When creating new kernel services:

- Prefer one dedicated kernel task per long-lived subsystem
- Use `scheduler_sleep` instead of busy loops
- Be aware that shared state needs protection once you add deeper concurrency primitives

Current limits:

- There are no mutexes, semaphores, condition variables, or wait queues yet
- All tasks share one address space
- Zombie tasks are not reaped yet

## Platform Services, Debug, And Files

### Console

See [`../kernel/arch/i386/vga_console.c`](../kernel/arch/i386/vga_console.c), [`../kernel/arch/i386/serial_console.c`](../kernel/arch/i386/serial_console.c), and [`../kernel/kernel/console.c`](../kernel/kernel/console.c).

The core kernel now writes through the generic console layer. The current backends:

- Use VGA text mode memory for on-screen logs
- Mirror the same output to COM1 for serial bring-up and debugging
- Are selected at boot by `platform_register_drivers`

### Input

See [`../kernel/arch/i386/ps2_keyboard.c`](../kernel/arch/i386/ps2_keyboard.c) and [`../kernel/kernel/input.c`](../kernel/kernel/input.c).

The current input driver:

- Assumes a PS/2 keyboard
- Decodes set-1 scancodes
- Buffers translated characters in a small ring buffer
- Reserves `F1` for debugger entry

Other kernel code should consume characters and debug requests through `input_read_char_nonblocking`, `input_debug_requested`, and `input_clear_debug_request`.

### Timer And IRQ Controller

See [`../kernel/arch/i386/pit_timer.c`](../kernel/arch/i386/pit_timer.c), [`../kernel/arch/i386/pic.c`](../kernel/arch/i386/pic.c), [`../kernel/kernel/timer.c`](../kernel/kernel/timer.c), and [`../kernel/kernel/irq.c`](../kernel/kernel/irq.c).

The current timer and interrupt-controller split works like this:

- The IRQ controller owns vector mapping, masking, and acknowledgement
- The timer driver owns hardware programming for the tick source
- Timer drivers register their tick handler against a logical IRQ line instead of assuming a fixed vector in generic code

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

## Writing New Hardware Modules

When you add support for different hardware, keep the generic kernel surface stable and swap only the platform implementation.

Recommended workflow:

1. Pick the smallest existing interface that fits the new device.
2. Implement the concrete driver under [`../kernel/arch/i386/`](../kernel/arch/i386/) or a new architecture directory if the work is not i386-specific.
3. Expose a `const` driver object matching the relevant interface type.
4. Expose a `const module_descriptor_t` that probes and activates that driver.
5. Register that module from [`../kernel/arch/i386/platform.c`](../kernel/arch/i386/platform.c) or the new platform's equivalent.
6. Add the new source file to the architecture build list in [`../kernel/arch/i386/make.config`](../kernel/arch/i386/make.config).
7. Validate the boot banners in [`../kernel/kernel/kernel.c`](../kernel/kernel/kernel.c), which print the active driver names and module counts for quick bring-up checks.

Rules to follow:

- Keep `x86_*`, raw port I/O, MMIO addresses, and device register definitions inside architecture-specific files.
- Keep policy and subsystem logic in generic kernel code whenever possible.
- Register IRQ handlers through `irq_register_handler` and enable lines through `irq_enable`.
- Keep boot-time hardware choice inside the module registry instead of scattering `if (has_hpet)` or `if (has_apic)` checks across generic code.
- Return `false` from driver `init` callbacks when the hardware cannot be set up cleanly.
- Do not let generic code depend on a specific vector number, I/O port, or device memory address.

Minimal timer-driver skeleton:

```c
#include <kernel/module.h>
#include <kernel/irq.h>
#include <kernel/timer.h>

static uint32_t my_timer_hz;

static bool my_timer_init(uint32_t frequency_hz, interrupt_handler_t tick_handler) {
	if (frequency_hz == 0 || tick_handler == NULL)
		return false;

	if (!irq_register_handler(IRQ_LINE_TIMER, tick_handler))
		return false;

	/* Program the hardware timer here. */
	irq_enable(IRQ_LINE_TIMER);
	my_timer_hz = frequency_hz;
	return true;
}

static uint32_t my_timer_frequency_hz(void) {
	return my_timer_hz;
}

const timer_driver_t my_timer_driver = {
	.name = "my timer",
	.init = my_timer_init,
	.frequency_hz = my_timer_frequency_hz,
};

static bool my_timer_activate(void) {
	timer_register_driver(&my_timer_driver);
	return true;
}

const module_descriptor_t my_timer_module = {
	.name = "my timer",
	.kind = MODULE_KIND_TIMER,
	.priority = 100u,
	.probe = NULL,
	.activate = my_timer_activate,
};
```

For a new input or console backend, the same pattern applies: implement the interface in an architecture-specific file, wrap it in a module descriptor, and let the rest of the kernel continue calling the generic service layer.

## Adding A New Platform

For a different hardware target, aim for a thin platform assembly layer plus reusable generic subsystems:

- Add a new `kernel/arch/<arch>/` tree with its own bootstrap, linker settings, and `make.config`
- Provide a platform registration function that registers that platform's console, input, IRQ controller, timer, and CPU modules
- Reuse `kernel/kernel/` code unless the subsystem truly depends on architecture behavior
- Extend the generic interfaces only when two platforms genuinely need a new shared capability

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

1. Per-process page-directory ownership and user/kernel address-space split
2. Locking primitives for shared kernel subsystems
3. Reaping and joining kernel tasks
4. A real vnode and directory-aware VFS
5. User and kernel privilege separation plus a syscall ABI
