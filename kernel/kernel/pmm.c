#include <stddef.h>
#include <stdint.h>

#include <kernel/boot.h>
#include <kernel/paging.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

#define PAGE_SIZE 4096u

static uint8_t* frame_bitmap;
static uint32_t total_frames;
static uint32_t free_frames;
static uint32_t total_memory_kib;
static uint32_t bitmap_end_phys;

static uint32_t align_up(uint32_t value, uint32_t alignment) {
	return (value + alignment - 1) & ~(alignment - 1);
}

static void bitmap_set(uint32_t frame) {
	frame_bitmap[frame / 8] |= (uint8_t) (1u << (frame % 8));
}

static void bitmap_clear(uint32_t frame) {
	frame_bitmap[frame / 8] &= (uint8_t) ~(1u << (frame % 8));
}

static int bitmap_test(uint32_t frame) {
	return (frame_bitmap[frame / 8] & (1u << (frame % 8))) != 0;
}

static void reserve_frame(uint32_t frame) {
	if (frame >= total_frames || bitmap_test(frame))
		return;
	bitmap_set(frame);
	if (free_frames > 0)
		free_frames--;
}

static void release_frame(uint32_t frame) {
	if (frame >= total_frames || !bitmap_test(frame))
		return;
	bitmap_clear(frame);
	free_frames++;
}

void pmm_mark_available_range(uint32_t base, uint32_t length) {
	const uint32_t start_frame = base / PAGE_SIZE;
	const uint32_t end_frame = (base + length) / PAGE_SIZE;

	for (uint32_t frame = start_frame; frame < end_frame && frame < total_frames; frame++)
		release_frame(frame);
}

void pmm_reserve_range(uint32_t base, uint32_t length) {
	const uint32_t start_frame = base / PAGE_SIZE;
	const uint32_t end_frame = align_up(base + length, PAGE_SIZE) / PAGE_SIZE;

	for (uint32_t frame = start_frame; frame < end_frame && frame < total_frames; frame++)
		reserve_frame(frame);
}

void pmm_init(uint32_t multiboot_info_addr) {
	const multiboot_info_t* mbi =
		(const multiboot_info_t*) paging_phys_to_virt(multiboot_info_addr);
	uint32_t highest_physical = 0;

	if ((mbi->flags & MULTIBOOT_INFO_MEM_MAP) != 0) {
		const uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
		for (uint32_t entry_addr = mbi->mmap_addr; entry_addr < mmap_end; ) {
			const multiboot_memory_map_t* entry =
				(const multiboot_memory_map_t*) paging_phys_to_virt(entry_addr);
			const uint32_t region_end = (uint32_t) (entry->addr + entry->len);
			if (region_end > highest_physical)
				highest_physical = region_end;
			entry_addr += entry->size + sizeof(entry->size);
		}
	} else if ((mbi->flags & MULTIBOOT_INFO_MEMORY) != 0) {
		highest_physical = (mbi->mem_upper + 1024u) * 1024u;
	} else {
		panic("no memory map provided by bootloader");
	}

	total_frames = align_up(highest_physical, PAGE_SIZE) / PAGE_SIZE;
	total_memory_kib = total_frames * 4u;

	const uint32_t kernel_end_phys = paging_virt_to_phys(&_kernel_end);
	const uint32_t bitmap_bytes = align_up((total_frames + 7u) / 8u, PAGE_SIZE);
	const uint32_t bitmap_phys = align_up(kernel_end_phys, PAGE_SIZE);

	bitmap_end_phys = bitmap_phys + bitmap_bytes;
	if (bitmap_end_phys >= paging_window_end_phys())
		panic("frame bitmap exceeds bootstrap mapping window");

	// The bitmap lives immediately after the kernel image inside the early bootstrap mapping.
	frame_bitmap = (uint8_t*) paging_phys_to_virt(bitmap_phys);
	for (uint32_t i = 0; i < bitmap_bytes; i++)
		frame_bitmap[i] = 0xFF;

	free_frames = 0;

	if ((mbi->flags & MULTIBOOT_INFO_MEM_MAP) != 0) {
		// Start from "everything reserved" and selectively free only Multiboot-available ranges.
		const uint32_t mmap_end = mbi->mmap_addr + mbi->mmap_length;
		for (uint32_t entry_addr = mbi->mmap_addr; entry_addr < mmap_end; ) {
			const multiboot_memory_map_t* entry =
				(const multiboot_memory_map_t*) paging_phys_to_virt(entry_addr);
			if (entry->type == MULTIBOOT_MEMORY_AVAILABLE)
				pmm_mark_available_range((uint32_t) entry->addr, (uint32_t) entry->len);
			entry_addr += entry->size + sizeof(entry->size);
		}
	}

	pmm_reserve_range(0, 0x100000);
	pmm_reserve_range((uint32_t) paging_virt_to_phys(&_kernel_start),
		kernel_end_phys - paging_virt_to_phys(&_kernel_start));
	pmm_reserve_range(bitmap_phys, bitmap_bytes);
	pmm_reserve_range(multiboot_info_addr, sizeof(multiboot_info_t));

	if ((mbi->flags & MULTIBOOT_INFO_MEM_MAP) != 0)
		pmm_reserve_range(mbi->mmap_addr, mbi->mmap_length);

	if ((mbi->flags & MULTIBOOT_INFO_MODULES) != 0) {
		const multiboot_module_t* modules =
			(const multiboot_module_t*) paging_phys_to_virt(mbi->mods_addr);
		// Modules stay pinned so the initrd and future boot modules are not recycled as free RAM.
		pmm_reserve_range(mbi->mods_addr, mbi->mods_count * sizeof(multiboot_module_t));

		for (uint32_t i = 0; i < mbi->mods_count; i++) {
			pmm_reserve_range(modules[i].mod_start, modules[i].mod_end - modules[i].mod_start);
			if (modules[i].string != 0)
				pmm_reserve_range(modules[i].string, 128);
		}
	}
}

bool pmm_alloc_frame(uint32_t* physical_addr_out) {
	for (uint32_t frame = 0; frame < total_frames; frame++) {
		if (!bitmap_test(frame)) {
			reserve_frame(frame);
			*physical_addr_out = frame * PAGE_SIZE;
			return true;
		}
	}
	return false;
}

void pmm_free_frame(uint32_t physical_addr) {
	release_frame(physical_addr / PAGE_SIZE);
}

bool pmm_alloc_frames(uint32_t frame_count, uint32_t* physical_addr_out) {
	uint32_t run_length = 0;
	uint32_t run_start = 0;

	// A naive first-fit contiguous search is enough for early paging experiments.
	for (uint32_t frame = 0; frame < total_frames; frame++) {
		if (bitmap_test(frame)) {
			run_length = 0;
			continue;
		}

		if (run_length == 0)
			run_start = frame;
		run_length++;

		if (run_length == frame_count) {
			for (uint32_t i = 0; i < frame_count; i++)
				reserve_frame(run_start + i);
			*physical_addr_out = run_start * PAGE_SIZE;
			return true;
		}
	}

	return false;
}

void pmm_free_frames(uint32_t physical_addr, uint32_t frame_count) {
	uint32_t frame = physical_addr / PAGE_SIZE;
	for (uint32_t i = 0; i < frame_count; i++)
		release_frame(frame + i);
}

uint32_t pmm_total_memory_kib(void) {
	return total_memory_kib;
}

uint32_t pmm_free_memory_kib(void) {
	return free_frames * 4u;
}

uint32_t pmm_bitmap_end_phys(void) {
	return bitmap_end_phys;
}
