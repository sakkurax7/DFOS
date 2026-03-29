#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/boot.h>
#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/x86.h>

extern uint32_t boot_page_directory[1024];
extern uint32_t boot_page_table1[1024];

static bool pae_supported;
static bool pae_ready;
static uint64_t pae_pdpt[4] __attribute__((aligned(32)));
static uint64_t pae_page_directory[512] __attribute__((aligned(4096)));
static uint64_t pae_page_tables[2][512] __attribute__((aligned(4096)));
static uint32_t next_dynamic_virtual = KERNEL_VMA + 0x00200000u;

static uint64_t make_pae_entry(uint32_t physical, uint64_t flags) {
	return ((uint64_t) physical & 0x00000000FFFFF000ULL) | flags;
}

void paging_init(uint32_t multiboot_info_addr) {
	(void) multiboot_info_addr;
	pae_supported = x86_cpu_has_pae();
	pae_ready = false;

	if (!pae_supported)
		return;

	// Build a minimal PAE hierarchy mirroring the current 4 MiB higher-half bootstrap window.
	for (uint32_t table = 0; table < 2; table++) {
		for (uint32_t entry = 0; entry < 512; entry++) {
			const uint32_t physical = (table * 512u + entry) * 0x1000u;
			pae_page_tables[table][entry] = make_pae_entry(physical, 0x003);
		}

		pae_page_directory[table] =
			make_pae_entry(paging_virt_to_phys(&pae_page_tables[table][0]), 0x003);
	}

	for (uint32_t entry = 2; entry < 512; entry++)
		pae_page_directory[entry] = 0;

	pae_pdpt[0] = make_pae_entry(paging_virt_to_phys(&pae_page_directory[0]), 0x003);
	pae_pdpt[1] = 0;
	pae_pdpt[2] = 0;
	pae_pdpt[3] = make_pae_entry(paging_virt_to_phys(&pae_page_directory[0]), 0x003);
	pae_ready = true;
}

bool paging_pae_supported(void) {
	return pae_supported;
}

bool paging_pae_ready(void) {
	return pae_ready;
}

const char* paging_mode_name(void) {
	return "legacy 32-bit paging";
}

void* paging_phys_to_virt(uint32_t physical) {
	return (void*) (physical + KERNEL_VMA);
}

uint32_t paging_virt_to_phys(const void* virtual_addr) {
	return (uint32_t) virtual_addr - KERNEL_VMA;
}

uint32_t paging_window_end_phys(void) {
	return BOOTSTRAP_WINDOW_SIZE;
}

static bool paging_is_bootstrap_virtual(const void* virtual_addr) {
	const uint32_t address = (uint32_t) virtual_addr;
	return address >= KERNEL_VMA && address < KERNEL_VMA + BOOTSTRAP_WINDOW_SIZE;
}

bool paging_map_page(void* virtual_addr, uint32_t physical_addr, uint32_t flags) {
	const uint32_t address = (uint32_t) virtual_addr;

	if (!paging_is_bootstrap_virtual(virtual_addr) || (address & 0xFFFu) != 0 ||
			(physical_addr & 0xFFFu) != 0)
		return false;

	// For now we only edit the single bootstrap page table that assembly installed at boot.
	const uint32_t page_index = (address - KERNEL_VMA) / 0x1000u;
	boot_page_table1[page_index] = (physical_addr & 0xFFFFF000u) | (flags & 0xFFFu) | 0x001u;
	asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
	return true;
}

void paging_unmap_page(void* virtual_addr) {
	if (!paging_is_bootstrap_virtual(virtual_addr))
		return;

	const uint32_t address = (uint32_t) virtual_addr;
	const uint32_t page_index = (address - KERNEL_VMA) / 0x1000u;
	boot_page_table1[page_index] = 0;
	asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

bool paging_is_mapped(const void* virtual_addr) {
	if (!paging_is_bootstrap_virtual(virtual_addr))
		return false;

	const uint32_t address = (uint32_t) virtual_addr;
	const uint32_t page_index = (address - KERNEL_VMA) / 0x1000u;
	return (boot_page_table1[page_index] & 0x001u) != 0;
}

void* paging_alloc_pages(uint32_t page_count) {
	void* base = (void*) next_dynamic_virtual;

	// This is a monotonic virtual allocator: pages can be unmapped later, but VA space is not reused.
	for (uint32_t i = 0; i < page_count; i++) {
		uint32_t frame;
		if (!pmm_alloc_frame(&frame))
			return NULL;

		if (!paging_map_page((void*) (next_dynamic_virtual + i * 0x1000u), frame, 0x003)) {
			pmm_free_frame(frame);
			return NULL;
		}
	}

	next_dynamic_virtual += page_count * 0x1000u;
	return base;
}

void paging_free_pages(void* base, uint32_t page_count) {
	for (uint32_t i = 0; i < page_count; i++) {
		void* address = (void*) ((uint32_t) base + i * 0x1000u);
		if (!paging_is_mapped(address))
			continue;

		pmm_free_frame(paging_virt_to_phys(address));
		paging_unmap_page(address);
	}
}
