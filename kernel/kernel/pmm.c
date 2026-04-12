#include <stddef.h>
#include <stdint.h>

#include <kernel/boot.h>
#include <kernel/bootinfo.h>
#include <kernel/paging.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

static uint8_t* frame_bitmap;
static uint32_t total_frames;
static uint32_t free_frames;
static uint32_t total_memory_kib;
static uint32_t bitmap_end_phys;
static bool initialized;

static uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
	return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t align_up_u64(uint64_t value, uint32_t alignment) {
	return (value + (uint64_t) alignment - 1u) & ~((uint64_t) alignment - 1u);
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
	const uint64_t end = (uint64_t) base + (uint64_t) length;
	const uint32_t end_frame = (uint32_t) (end / PAGE_SIZE);

	for (uint32_t frame = start_frame; frame < end_frame && frame < total_frames; frame++)
		release_frame(frame);
}

void pmm_reserve_range(uint32_t base, uint32_t length) {
	const uint32_t start_frame = base / PAGE_SIZE;
	const uint64_t end = align_up_u64((uint64_t) base + (uint64_t) length, PAGE_SIZE);
	const uint32_t end_frame = (uint32_t) (end / PAGE_SIZE);

	for (uint32_t frame = start_frame; frame < end_frame && frame < total_frames; frame++)
		reserve_frame(frame);
}

// The current 32-bit allocator tracks addresses in uint32_t and therefore caps
// managed space at the last page-aligned physical address below 4 GiB.
#define PMM_PHYS_CAP ((uint64_t) 0xFFFFF000u)

void pmm_init(void) {
	initialized = false;
	uint32_t highest_physical = 0;

	if (bootinfo_has_memory_map()) {
		bootinfo_memory_iterator_t iterator;
		bootinfo_memory_region_t region;

		for (bool has_entry = bootinfo_memory_begin(&iterator, &region);
				has_entry; has_entry = bootinfo_memory_next(&iterator, &region)) {
			uint64_t region_end = region.base + region.length;
			if (region_end < region.base)
				region_end = PMM_PHYS_CAP;
			if (region_end > PMM_PHYS_CAP)
				region_end = PMM_PHYS_CAP;

			if (region_end == 0)
				continue;

			const uint32_t region_end_u32 = (uint32_t) region_end;
			if (region_end_u32 > highest_physical)
				highest_physical = region_end_u32;
		}
	} else if (bootinfo_has_memory_kib_info()) {
		highest_physical = (bootinfo_upper_memory_kib() + 1024u) * 1024u;
	} else {
		panic("no memory map provided by bootloader");
	}

	if (highest_physical == 0)
		panic("bootloader reported zero physical memory");

	total_frames = align_up_u32(highest_physical, PAGE_SIZE) / PAGE_SIZE;
	total_memory_kib = total_frames * 4u;

	const uint32_t kernel_end_phys = paging_virt_to_phys(&_kernel_end);
	const uint32_t bitmap_bytes = align_up_u32((total_frames + 7u) / 8u, PAGE_SIZE);
	const uint32_t bitmap_phys = align_up_u32(kernel_end_phys, PAGE_SIZE);

	bitmap_end_phys = bitmap_phys + bitmap_bytes;
	if (bitmap_end_phys >= paging_window_end_phys())
		panic("frame bitmap exceeds bootstrap mapping window");

	// The bitmap lives immediately after the kernel image inside the early bootstrap mapping.
	frame_bitmap = (uint8_t*) paging_phys_to_virt(bitmap_phys);
	for (uint32_t i = 0; i < bitmap_bytes; i++)
		frame_bitmap[i] = 0xFF;

	free_frames = 0;

	if (bootinfo_has_memory_map()) {
		// Start from "everything reserved" and selectively free only bootloader-available ranges.
		bootinfo_memory_iterator_t iterator;
		bootinfo_memory_region_t region;
		for (bool has_entry = bootinfo_memory_begin(&iterator, &region);
				has_entry; has_entry = bootinfo_memory_next(&iterator, &region)) {
			if (region.type != MULTIBOOT_MEMORY_AVAILABLE || region.length == 0)
				continue;

			uint64_t start = region.base;
			uint64_t end = region.base + region.length;
			if (end <= start)
				continue;

			if (start >= PMM_PHYS_CAP)
				continue;
			if (end > PMM_PHYS_CAP)
				end = PMM_PHYS_CAP;

			pmm_mark_available_range((uint32_t) start, (uint32_t) (end - start));
		}
	}

	pmm_reserve_range(0, 0x100000);
	pmm_reserve_range((uint32_t) paging_virt_to_phys(&_kernel_start),
		kernel_end_phys - paging_virt_to_phys(&_kernel_start));
	pmm_reserve_range(bitmap_phys, bitmap_bytes);

	if (bootinfo_info_phys() != 0 && bootinfo_info_size_hint() != 0)
		pmm_reserve_range(bootinfo_info_phys(), bootinfo_info_size_hint());

	if (bootinfo_has_memory_map())
		pmm_reserve_range(bootinfo_memory_map_phys(), bootinfo_memory_map_size());

	const uint32_t module_table_phys = bootinfo_module_table_phys();
	const uint32_t module_table_size = bootinfo_module_table_size();
	if (module_table_phys != 0 && module_table_size != 0)
		pmm_reserve_range(module_table_phys, module_table_size);

	const uint32_t module_count = bootinfo_module_count();
	for (uint32_t i = 0; i < module_count; i++) {
		bootinfo_module_t module;
		if (!bootinfo_module_at(i, &module))
			continue;

		if (module.end > module.start)
			pmm_reserve_range(module.start, module.end - module.start);

		if (module.string != NULL)
			pmm_reserve_range(paging_virt_to_phys(module.string), 128);
	}

	initialized = true;
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

bool pmm_is_initialized(void) {
	return initialized;
}

uint32_t pmm_bitmap_end_phys(void) {
	return bitmap_end_phys;
}
