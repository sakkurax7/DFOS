#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <stdbool.h>
#include <stdint.h>

#define PMM_MAX_NUMA_NODES 8u
#define PMM_NUMA_NODE_ANY  0xFFu

typedef struct pmm_numa_node_stats {
	uint8_t node_id;
	uint32_t first_frame;
	uint32_t frame_count;
	uint32_t free_frames;
} pmm_numa_node_stats_t;

void pmm_init(void);
bool pmm_configure_numa_topology(uint8_t node_count);
uint8_t pmm_numa_node_count(void);
bool pmm_get_numa_node_stats(uint8_t node_id, pmm_numa_node_stats_t* stats);
uint8_t pmm_numa_node_for_physical(uint32_t physical_addr);
bool pmm_alloc_frame(uint32_t* physical_addr_out);
bool pmm_alloc_frame_on_node(uint8_t preferred_node, uint32_t* physical_addr_out);
void pmm_free_frame(uint32_t physical_addr);
bool pmm_alloc_frames(uint32_t frame_count, uint32_t* physical_addr_out);
bool pmm_alloc_frames_on_node(
	uint32_t frame_count, uint8_t preferred_node, uint32_t* physical_addr_out);
void pmm_free_frames(uint32_t physical_addr, uint32_t frame_count);
void pmm_reserve_range(uint32_t base, uint32_t length);
void pmm_mark_available_range(uint32_t base, uint32_t length);
bool pmm_is_initialized(void);
uint32_t pmm_total_memory_kib(void);
uint32_t pmm_free_memory_kib(void);
uint32_t pmm_bitmap_end_phys(void);

#endif
