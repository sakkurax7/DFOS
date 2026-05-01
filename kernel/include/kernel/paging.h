#ifndef KERNEL_PAGING_H
#define KERNEL_PAGING_H

#include <stdbool.h>
#include <stdint.h>

#define KERNEL_VMA            0xC0000000u
#define BOOTSTRAP_WINDOW_SIZE 0x00400000u
#define PAGE_SIZE             0x00001000u
#define PAGING_USER_BASE      0x00400000u
#define PAGING_USER_LIMIT     KERNEL_VMA

#define PAGING_FLAG_PRESENT   0x001u
#define PAGING_FLAG_WRITABLE  0x002u
#define PAGING_FLAG_USER      0x004u

typedef struct paging_space paging_space_t;

void paging_init(uint32_t multiboot_info_addr);
void paging_finalize_bootstrap(void);
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
void* paging_alloc_pages_on_node(uint32_t page_count, uint8_t preferred_numa_node);
void paging_free_pages(void* base, uint32_t page_count);

paging_space_t* paging_kernel_space(void);
paging_space_t* paging_current_space(void);
paging_space_t* paging_create_process_space(void);
paging_space_t* paging_create_process_space_on_node(uint8_t preferred_numa_node);
void paging_destroy_process_space(paging_space_t* space);
void paging_switch_space(paging_space_t* space);
uint32_t paging_space_root_physical(const paging_space_t* space);
bool paging_map_user_page(paging_space_t* space, void* virtual_addr,
	uint32_t physical_addr, uint32_t flags);
void paging_unmap_user_page(paging_space_t* space, void* virtual_addr);
bool paging_lookup_physical(paging_space_t* space, const void* virtual_addr, uint32_t* physical_out);

#endif
