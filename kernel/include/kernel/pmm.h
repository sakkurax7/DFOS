#ifndef KERNEL_PMM_H
#define KERNEL_PMM_H

#include <stdbool.h>
#include <stdint.h>

void pmm_init(uint32_t multiboot_info_addr);
bool pmm_alloc_frame(uint32_t* physical_addr_out);
void pmm_free_frame(uint32_t physical_addr);
bool pmm_alloc_frames(uint32_t frame_count, uint32_t* physical_addr_out);
void pmm_free_frames(uint32_t physical_addr, uint32_t frame_count);
void pmm_reserve_range(uint32_t base, uint32_t length);
void pmm_mark_available_range(uint32_t base, uint32_t length);
bool pmm_is_initialized(void);
uint32_t pmm_total_memory_kib(void);
uint32_t pmm_free_memory_kib(void);
uint32_t pmm_bitmap_end_phys(void);

#endif
