#include <stddef.h>
#include <stdint.h>

#include <kernel/boot.h>
#include <kernel/bootinfo.h>
#include <kernel/paging.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/x86.h>

extern uint8_t _kernel_start;
extern uint8_t _kernel_end;

static uint8_t* frame_bitmap;
static uint32_t total_frames;
static uint32_t free_frames;
static uint32_t total_memory_kib;
static uint32_t bitmap_end_phys;
static bool initialized;
static volatile uint32_t pmm_state_lock;

typedef struct pmm_numa_node {
	uint32_t first_frame;
	uint32_t frame_count;
	uint32_t free_frames;
	uint32_t next_scan_frame;
} pmm_numa_node_t;

static pmm_numa_node_t numa_nodes[PMM_MAX_NUMA_NODES];
static uint8_t active_numa_node_count;
static uint8_t round_robin_numa_node;
static uint32_t global_next_scan_frame;

#define X86_EFLAGS_INTERRUPT_FLAG (1u << 9)

static uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
	return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t align_up_u64(uint64_t value, uint32_t alignment) {
	return (value + (uint64_t) alignment - 1u) & ~((uint64_t) alignment - 1u);
}

static uint32_t pmm_irq_save(void) {
	const uint32_t flags = x86_read_eflags();
	x86_cli();
	return flags;
}

static void pmm_irq_restore(uint32_t flags) {
	if ((flags & X86_EFLAGS_INTERRUPT_FLAG) != 0)
		x86_sti();
}

static bool pmm_atomic_try_lock_u32(volatile uint32_t* target) {
	uint32_t desired = 1u;
	asm volatile("xchgl %0, %1"
		: "+r"(desired), "+m"(*target)
		:
		: "memory");
	return desired == 0u;
}

static uint32_t pmm_lock_irqsave(void) {
	const uint32_t irq_flags = pmm_irq_save();

	for (;;) {
		if (pmm_atomic_try_lock_u32(&pmm_state_lock))
			return irq_flags;

		while (pmm_state_lock != 0u)
			x86_pause();
	}
}

static void pmm_unlock_irqrestore(uint32_t irq_flags) {
	asm volatile("" : : : "memory");
	pmm_state_lock = 0;
	pmm_irq_restore(irq_flags);
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

static bool pmm_node_contains_frame(const pmm_numa_node_t* node, uint32_t frame) {
	if (node == NULL || node->frame_count == 0)
		return false;

	const uint32_t end = node->first_frame + node->frame_count;
	return frame >= node->first_frame && frame < end;
}

static uint8_t pmm_node_index_for_frame(uint32_t frame) {
	for (uint32_t node = 0; node < active_numa_node_count; node++) {
		if (pmm_node_contains_frame(&numa_nodes[node], frame))
			return (uint8_t) node;
	}

	return PMM_NUMA_NODE_ANY;
}

static void pmm_numa_reset_layout(uint8_t node_count) {
	if (node_count == 0)
		node_count = 1;

	if (node_count > PMM_MAX_NUMA_NODES)
		node_count = PMM_MAX_NUMA_NODES;

	const uint32_t base_frames_per_node = total_frames / node_count;
	uint32_t remainder = total_frames % node_count;
	uint32_t cursor = 0;

	for (uint32_t node = 0; node < node_count; node++) {
		const uint32_t frames = base_frames_per_node + (remainder > 0 ? 1u : 0u);
		if (remainder > 0)
			remainder--;

		numa_nodes[node].first_frame = cursor;
		numa_nodes[node].frame_count = frames;
		numa_nodes[node].free_frames = 0;
		numa_nodes[node].next_scan_frame = cursor;
		cursor += frames;
	}

	for (uint32_t node = node_count; node < PMM_MAX_NUMA_NODES; node++) {
		numa_nodes[node].first_frame = 0;
		numa_nodes[node].frame_count = 0;
		numa_nodes[node].free_frames = 0;
		numa_nodes[node].next_scan_frame = 0;
	}

	active_numa_node_count = node_count;
	if (round_robin_numa_node >= active_numa_node_count)
		round_robin_numa_node = 0;
	if (global_next_scan_frame >= total_frames)
		global_next_scan_frame = 0;
}

static void pmm_recount_numa_free_frames_locked(void) {
	uint32_t total_free = 0;

	for (uint32_t node = 0; node < active_numa_node_count; node++) {
		pmm_numa_node_t* numa_node = &numa_nodes[node];
		uint32_t node_free = 0;

		const uint32_t start = numa_node->first_frame;
		const uint32_t end = start + numa_node->frame_count;
		for (uint32_t frame = start; frame < end; frame++) {
			if (!bitmap_test(frame))
				node_free++;
		}

		numa_node->free_frames = node_free;
		total_free += node_free;

		if (!pmm_node_contains_frame(numa_node, numa_node->next_scan_frame))
			numa_node->next_scan_frame = start;
	}

	free_frames = total_free;
}

static bool reserve_frame_locked(uint32_t frame) {
	if (frame >= total_frames || bitmap_test(frame))
		return false;

	bitmap_set(frame);
	if (free_frames > 0)
		free_frames--;

	const uint8_t node = pmm_node_index_for_frame(frame);
	if (node != PMM_NUMA_NODE_ANY && numa_nodes[node].free_frames > 0)
		numa_nodes[node].free_frames--;

	return true;
}

static bool release_frame_locked(uint32_t frame) {
	if (frame >= total_frames || !bitmap_test(frame))
		return false;

	bitmap_clear(frame);
	free_frames++;

	const uint8_t node = pmm_node_index_for_frame(frame);
	if (node != PMM_NUMA_NODE_ANY)
		numa_nodes[node].free_frames++;

	return true;
}

static bool pmm_find_free_frame_in_node_locked(uint8_t node_id, uint32_t* frame_out) {
	if (node_id >= active_numa_node_count || frame_out == NULL)
		return false;

	pmm_numa_node_t* node = &numa_nodes[node_id];
	if (node->frame_count == 0 || node->free_frames == 0)
		return false;

	const uint32_t start = node->first_frame;
	const uint32_t end = start + node->frame_count;
	uint32_t cursor = node->next_scan_frame;
	if (!pmm_node_contains_frame(node, cursor))
		cursor = start;

	for (uint32_t frame = cursor; frame < end; frame++) {
		if (bitmap_test(frame))
			continue;

		if (!reserve_frame_locked(frame))
			continue;

		node->next_scan_frame = frame + 1u < end ? frame + 1u : start;
		*frame_out = frame;
		return true;
	}

	for (uint32_t frame = start; frame < cursor; frame++) {
		if (bitmap_test(frame))
			continue;

		if (!reserve_frame_locked(frame))
			continue;

		node->next_scan_frame = frame + 1u < end ? frame + 1u : start;
		*frame_out = frame;
		return true;
	}

	return false;
}

static bool pmm_find_contiguous_free_linear_locked(
	uint32_t start,
	uint32_t end,
	uint32_t frame_count,
	uint32_t* run_start_out) {
	if (start >= end || frame_count == 0 || run_start_out == NULL)
		return false;

	if ((end - start) < frame_count)
		return false;

	uint32_t run_start = 0;
	uint32_t run_length = 0;

	for (uint32_t frame = start; frame < end; frame++) {
		if (bitmap_test(frame)) {
			run_length = 0;
			continue;
		}

		if (run_length == 0)
			run_start = frame;
		run_length++;

		if (run_length == frame_count) {
			*run_start_out = run_start;
			return true;
		}
	}

	return false;
}

static bool pmm_find_contiguous_free_wrapped_locked(
	uint32_t start,
	uint32_t end,
	uint32_t frame_count,
	uint32_t search_start,
	uint32_t* run_start_out) {
	if (start >= end || frame_count == 0 || run_start_out == NULL)
		return false;

	if ((end - start) < frame_count)
		return false;

	if (search_start < start || search_start >= end)
		search_start = start;

	if (pmm_find_contiguous_free_linear_locked(search_start, end, frame_count, run_start_out))
		return true;

	if (search_start == start)
		return false;

	return pmm_find_contiguous_free_linear_locked(start, search_start, frame_count, run_start_out);
}

static bool pmm_reserve_contiguous_locked(uint32_t start_frame, uint32_t frame_count) {
	for (uint32_t i = 0; i < frame_count; i++) {
		if (!reserve_frame_locked(start_frame + i))
			return false;
	}

	return true;
}

static bool pmm_find_contiguous_in_node_locked(
	uint8_t node_id, uint32_t frame_count, uint32_t* run_start_out) {
	if (node_id >= active_numa_node_count || run_start_out == NULL || frame_count == 0)
		return false;

	pmm_numa_node_t* node = &numa_nodes[node_id];
	if (node->frame_count < frame_count || node->free_frames < frame_count)
		return false;

	const uint32_t start = node->first_frame;
	const uint32_t end = start + node->frame_count;
	uint32_t cursor = node->next_scan_frame;
	if (!pmm_node_contains_frame(node, cursor))
		cursor = start;

	uint32_t run_start;
	if (!pmm_find_contiguous_free_wrapped_locked(start, end, frame_count, cursor, &run_start))
		return false;

	if (!pmm_reserve_contiguous_locked(run_start, frame_count))
		return false;

	const uint32_t node_end = start + node->frame_count;
	node->next_scan_frame = run_start + frame_count;
	if (node->next_scan_frame >= node_end)
		node->next_scan_frame = start;

	*run_start_out = run_start;
	return true;
}

void pmm_mark_available_range(uint32_t base, uint32_t length) {
	const uint32_t irq_flags = pmm_lock_irqsave();
	const uint32_t start_frame = base / PAGE_SIZE;
	const uint64_t end = (uint64_t) base + (uint64_t) length;
	const uint32_t end_frame = (uint32_t) (end / PAGE_SIZE);

	for (uint32_t frame = start_frame; frame < end_frame && frame < total_frames; frame++)
		(void) release_frame_locked(frame);
	pmm_unlock_irqrestore(irq_flags);
}

void pmm_reserve_range(uint32_t base, uint32_t length) {
	const uint32_t irq_flags = pmm_lock_irqsave();
	const uint32_t start_frame = base / PAGE_SIZE;
	const uint64_t end = align_up_u64((uint64_t) base + (uint64_t) length, PAGE_SIZE);
	const uint32_t end_frame = (uint32_t) (end / PAGE_SIZE);

	for (uint32_t frame = start_frame; frame < end_frame && frame < total_frames; frame++)
		(void) reserve_frame_locked(frame);
	pmm_unlock_irqrestore(irq_flags);
}

// The current 32-bit allocator tracks addresses in uint32_t and therefore caps
// managed space at the last page-aligned physical address below 4 GiB.
#define PMM_PHYS_CAP ((uint64_t) 0xFFFFF000u)

void pmm_init(void) {
	initialized = false;
	pmm_state_lock = 0;
	active_numa_node_count = 1;
	round_robin_numa_node = 0;
	global_next_scan_frame = 0;
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
	pmm_numa_reset_layout(1);

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

	const uint32_t irq_flags = pmm_lock_irqsave();
	pmm_recount_numa_free_frames_locked();
	pmm_unlock_irqrestore(irq_flags);
	initialized = true;
}

bool pmm_alloc_frame(uint32_t* physical_addr_out) {
	return pmm_alloc_frame_on_node(PMM_NUMA_NODE_ANY, physical_addr_out);
}

bool pmm_alloc_frame_on_node(uint8_t preferred_node, uint32_t* physical_addr_out) {
	if (physical_addr_out == NULL)
		return false;

	const uint32_t irq_flags = pmm_lock_irqsave();
	bool success = false;
	uint32_t selected_frame = 0;

	if (!initialized || active_numa_node_count == 0 || free_frames == 0)
		goto done;

	if (preferred_node != PMM_NUMA_NODE_ANY && preferred_node < active_numa_node_count) {
		if (pmm_find_free_frame_in_node_locked(preferred_node, &selected_frame)) {
			round_robin_numa_node = (preferred_node + 1u) % active_numa_node_count;
			success = true;
			goto done;
		}
	}

	const uint8_t start_node = round_robin_numa_node % active_numa_node_count;
	for (uint32_t i = 0; i < active_numa_node_count; i++) {
		const uint8_t node = (uint8_t) ((start_node + i) % active_numa_node_count);
		if (preferred_node != PMM_NUMA_NODE_ANY &&
			preferred_node < active_numa_node_count &&
			node == preferred_node)
			continue;

		if (!pmm_find_free_frame_in_node_locked(node, &selected_frame))
			continue;

		round_robin_numa_node = (node + 1u) % active_numa_node_count;
		success = true;
		break;
	}

done:
	if (success)
		*physical_addr_out = selected_frame * PAGE_SIZE;

	pmm_unlock_irqrestore(irq_flags);
	return success;
}

void pmm_free_frame(uint32_t physical_addr) {
	const uint32_t irq_flags = pmm_lock_irqsave();
	(void) release_frame_locked(physical_addr / PAGE_SIZE);
	pmm_unlock_irqrestore(irq_flags);
}

bool pmm_alloc_frames(uint32_t frame_count, uint32_t* physical_addr_out) {
	return pmm_alloc_frames_on_node(frame_count, PMM_NUMA_NODE_ANY, physical_addr_out);
}

bool pmm_alloc_frames_on_node(
	uint32_t frame_count, uint8_t preferred_node, uint32_t* physical_addr_out) {
	if (frame_count == 0 || physical_addr_out == NULL)
		return false;

	const uint32_t irq_flags = pmm_lock_irqsave();
	bool success = false;
	uint32_t run_start = 0;

	if (!initialized || active_numa_node_count == 0 || free_frames < frame_count)
		goto done;

	if (preferred_node != PMM_NUMA_NODE_ANY && preferred_node < active_numa_node_count) {
		if (pmm_find_contiguous_in_node_locked(preferred_node, frame_count, &run_start)) {
			round_robin_numa_node = (preferred_node + 1u) % active_numa_node_count;
			success = true;
			goto done;
		}
	}

	const uint8_t start_node = round_robin_numa_node % active_numa_node_count;
	for (uint32_t i = 0; i < active_numa_node_count; i++) {
		const uint8_t node = (uint8_t) ((start_node + i) % active_numa_node_count);
		if (preferred_node != PMM_NUMA_NODE_ANY &&
			preferred_node < active_numa_node_count &&
			node == preferred_node)
			continue;

		if (!pmm_find_contiguous_in_node_locked(node, frame_count, &run_start))
			continue;

		round_robin_numa_node = (node + 1u) % active_numa_node_count;
		success = true;
		break;
	}

	if (!success) {
		if (!pmm_find_contiguous_free_wrapped_locked(
				0, total_frames, frame_count, global_next_scan_frame, &run_start))
			goto done;

		if (!pmm_reserve_contiguous_locked(run_start, frame_count))
			goto done;

		success = true;
	}

	if (success) {
		global_next_scan_frame = run_start + frame_count;
		if (global_next_scan_frame >= total_frames)
			global_next_scan_frame %= total_frames;

		const uint8_t node = pmm_node_index_for_frame(run_start);
		if (node != PMM_NUMA_NODE_ANY && active_numa_node_count > 0)
			round_robin_numa_node = (node + 1u) % active_numa_node_count;
	}

done:
	if (success)
		*physical_addr_out = run_start * PAGE_SIZE;

	pmm_unlock_irqrestore(irq_flags);
	return success;
}

void pmm_free_frames(uint32_t physical_addr, uint32_t frame_count) {
	const uint32_t irq_flags = pmm_lock_irqsave();
	uint32_t frame = physical_addr / PAGE_SIZE;
	for (uint32_t i = 0; i < frame_count; i++)
		(void) release_frame_locked(frame + i);
	pmm_unlock_irqrestore(irq_flags);
}

bool pmm_configure_numa_topology(uint8_t node_count) {
	if (node_count == 0 || node_count > PMM_MAX_NUMA_NODES)
		return false;

	const uint32_t irq_flags = pmm_lock_irqsave();
	pmm_numa_reset_layout(node_count);
	if (frame_bitmap != NULL && total_frames > 0)
		pmm_recount_numa_free_frames_locked();
	pmm_unlock_irqrestore(irq_flags);
	return true;
}

uint8_t pmm_numa_node_count(void) {
	const uint32_t irq_flags = pmm_lock_irqsave();
	const uint8_t node_count = active_numa_node_count;
	pmm_unlock_irqrestore(irq_flags);
	return node_count;
}

bool pmm_get_numa_node_stats(uint8_t node_id, pmm_numa_node_stats_t* stats) {
	if (stats == NULL)
		return false;

	const uint32_t irq_flags = pmm_lock_irqsave();
	if (node_id >= active_numa_node_count) {
		pmm_unlock_irqrestore(irq_flags);
		return false;
	}

	const pmm_numa_node_t* node = &numa_nodes[node_id];
	stats->node_id = node_id;
	stats->first_frame = node->first_frame;
	stats->frame_count = node->frame_count;
	stats->free_frames = node->free_frames;
	pmm_unlock_irqrestore(irq_flags);
	return true;
}

uint8_t pmm_numa_node_for_physical(uint32_t physical_addr) {
	const uint32_t frame = physical_addr / PAGE_SIZE;
	const uint32_t irq_flags = pmm_lock_irqsave();
	if (frame >= total_frames) {
		pmm_unlock_irqrestore(irq_flags);
		return PMM_NUMA_NODE_ANY;
	}

	const uint8_t node = pmm_node_index_for_frame(frame);
	pmm_unlock_irqrestore(irq_flags);
	return node;
}

uint32_t pmm_total_memory_kib(void) {
	return total_memory_kib;
}

uint32_t pmm_free_memory_kib(void) {
	const uint32_t irq_flags = pmm_lock_irqsave();
	const uint32_t free_kib = free_frames * 4u;
	pmm_unlock_irqrestore(irq_flags);
	return free_kib;
}

bool pmm_is_initialized(void) {
	return initialized;
}

uint32_t pmm_bitmap_end_phys(void) {
	return bitmap_end_phys;
}
