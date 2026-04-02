#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <kernel/boot.h>
#include <kernel/console.h>
#include <kernel/cpu.h>
#include <kernel/gdt.h>
#include <kernel/heap.h>
#include <kernel/input.h>
#include <kernel/interrupts.h>
#include <kernel/irq.h>
#include <kernel/kdebug.h>
#include <kernel/paging.h>
#include <kernel/panic.h>
#include <kernel/platform.h>
#include <kernel/pmm.h>
#include <kernel/scheduler.h>
#include <kernel/stack_protector.h>
#include <kernel/timer.h>
#include <kernel/vfs.h>

static void worker_task(void* arg) {
	const char* name = (const char*) arg;
	uint32_t counter = 0;

	while (true) {
		// Demo worker threads give the scheduler, timer, and printf paths visible activity.
		printf("[task:%s] heartbeat %u (tick %u)\n", name, counter++, scheduler_ticks());
		scheduler_sleep(25);
	}
}

void kernel_main(uint32_t multiboot_magic, uint32_t multiboot_info_addr) {
	platform_register_drivers();
	if (!console_initialize()) {
		while (true)
			cpu_halt();
	}
	printf("DFOS kernel bootstrap\n");
	// Seed the canary as early as possible so stack-protected code is safe immediately.
	stack_protector_init(multiboot_magic, multiboot_info_addr);

	if (multiboot_magic != MULTIBOOT_BOOTLOADER_MAGIC)
		panic("invalid multiboot magic 0x%x", multiboot_magic);

	gdt_init();
	idt_init();
	paging_init(multiboot_info_addr);
	pmm_init(multiboot_info_addr);
	heap_init();
	if (!input_initialize())
		panic("failed to initialize input driver");
	vfs_init(multiboot_info_addr);
	scheduler_init();
	if (!timer_initialize(100, scheduler_on_timer_tick))
		panic("failed to initialize timer driver");
	scheduler_bootstrap_current("bootstrap");
	kdebug_init();

	// These boot-time banners are the quickest sanity check when bringing up new hardware code.
	printf("physical memory: %u KiB total, %u KiB free\n",
		pmm_total_memory_kib(), pmm_free_memory_kib());
	printf("heap window: %p - %p\n", heap_start(), heap_end());
	printf("paging mode: %s (PAE supported: %s)\n",
		paging_mode_name(), paging_pae_supported() ? "yes" : "no");
	printf("PAE tables: %s\n", paging_pae_ready() ? "prepared" : "unavailable");
	printf("initrd files: %u\n", vfs_file_count());
	printf("console: %s\n", console_driver_name());
	printf("input: %s\n", input_driver_name());
	printf("timer: %s @ %u Hz\n", timer_driver_name(), timer_frequency_hz());
	printf("irq controller: %s\n", irq_controller_name());
	printf("debug hotkey: F1\n");

	scheduler_create_kernel_task("worker-a", worker_task, "worker-a");
	scheduler_create_kernel_task("worker-b", worker_task, "worker-b");

	printf("interrupts online, scheduler started\n");
	interrupts_enable();

	while (true)
		// The idle path simply waits for the next interrupt.
		cpu_halt();
}
