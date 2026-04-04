#ifndef KERNEL_PAGING_H
#define KERNEL_PAGING_H

#include <stdbool.h>
#include <stdint.h>

#define KERNEL_VMA            0xC0000000u
#define BOOTSTRAP_WINDOW_SIZE 0x00400000u
#define PAGE_SIZE             0x00001000u

#define PAGING_FLAG_PRESENT   0x001u
#define PAGING_FLAG_WRITABLE  0x002u
#define PAGING_FLAG_USER      0x004u

void paging_init(uint32_t multiboot_info_addr);
bool paging_pae_supported(void);
bool paging_pae_ready(void);
const char* paging_mode_name(void);
void* paging_phys_to_virt(uint32_t physical);
uint32_t paging_virt_to_phys(const void* virtual_addr);
uint32_t paging_window_end_phys(void);
bool paging_map_page(void* virtual_addr, uint32_t physical_addr, uint32_t flags);
void paging_unmap_page(void* virtual_addr);
bool paging_is_mapped(const void* virtual_addr);
void* paging_alloc_pages(uint32_t page_count);
void paging_free_pages(void* base, uint32_t page_count);

#endif
