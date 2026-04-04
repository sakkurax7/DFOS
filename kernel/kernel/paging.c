#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/cpu.h>
#include <kernel/paging.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/x86.h>

extern uint32_t boot_page_table_pool[];
extern uint32_t boot_page_table_pool_end[];

extern uint8_t _text_start;
extern uint8_t _text_end;
extern uint8_t _rodata_start;
extern uint8_t _rodata_end;

extern void i386_pae_trampoline(uint32_t pdpt_physical, uint32_t low_stack,
	uint32_t high_stack);

static bool pae_supported;
static bool pae_ready;
static bool pae_active;

typedef enum paging_runtime_mode {
	PAGING_MODE_LEGACY = 0,
	PAGING_MODE_PAE
} paging_runtime_mode_t;

static paging_runtime_mode_t paging_mode = PAGING_MODE_LEGACY;

#define KERNEL_DIRECT_MAP_LIMIT_PHYS   0x10000000u
#define KERNEL_DYNAMIC_BASE            0xF0000000u
#define KERNEL_DYNAMIC_LIMIT           0xFFC00000u

#define PAGE_PRESENT                   PAGING_FLAG_PRESENT
#define PAGE_WRITABLE                  PAGING_FLAG_WRITABLE
#define PAGE_USER                      PAGING_FLAG_USER

static uint32_t next_dynamic_virtual = KERNEL_DYNAMIC_BASE;

static uint32_t align_down_u32(uint32_t value, uint32_t alignment) {
	return value & ~(alignment - 1u);
}

static uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
	return (value + alignment - 1u) & ~(alignment - 1u);
}

static void paging_invalidate_page(const void* virtual_addr) {
	asm volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
}

static void paging_flush_tlb(void) {
	x86_write_cr3(x86_read_cr3());
}

// -----------------------------------------------------------------------------
// Legacy 32-bit paging (fallback path when PAE is unavailable)
// -----------------------------------------------------------------------------

#define LEGACY_PDE_ENTRIES             1024u
#define LEGACY_PTE_ENTRIES             1024u
#define LEGACY_RECURSIVE_PDE_INDEX     1023u
#define LEGACY_PT_RECURSIVE_BASE       0xFFC00000u
#define LEGACY_PD_RECURSIVE_BASE       0xFFFFF000u

static volatile uint32_t* const legacy_recursive_page_directory =
	(volatile uint32_t*) LEGACY_PD_RECURSIVE_BASE;
static bool legacy_table_backed_by_pmm[LEGACY_PDE_ENTRIES];
static uint32_t legacy_early_table_count;
static uint32_t legacy_next_early_table;

static uint32_t legacy_directory_index_of(uint32_t virtual_addr) {
	return virtual_addr >> 22;
}

static uint32_t legacy_table_index_of(uint32_t virtual_addr) {
	return (virtual_addr >> 12) & 0x3FFu;
}

static volatile uint32_t* legacy_page_table_from_directory_index(uint32_t directory_index) {
	return (volatile uint32_t*) (LEGACY_PT_RECURSIVE_BASE + directory_index * PAGE_SIZE);
}

static bool legacy_alloc_page_table_frame(uint32_t* physical_out, bool* backed_by_pmm_out) {
	if (pmm_is_initialized()) {
		if (!pmm_alloc_frame(physical_out))
			return false;
		*backed_by_pmm_out = true;
		return true;
	}

	if (legacy_next_early_table >= legacy_early_table_count)
		return false;

	const uintptr_t table_virtual =
		(uintptr_t) &boot_page_table_pool[legacy_next_early_table * LEGACY_PTE_ENTRIES];
	legacy_next_early_table++;
	*physical_out = (uint32_t) (table_virtual - KERNEL_VMA);
	*backed_by_pmm_out = false;
	return true;
}

static volatile uint32_t* legacy_get_page_table(uint32_t virtual_addr, bool create,
		uint32_t page_flags) {
	const uint32_t dir = legacy_directory_index_of(virtual_addr);

	if (dir == LEGACY_RECURSIVE_PDE_INDEX)
		return NULL;

	uint32_t pde = legacy_recursive_page_directory[dir];

	if ((pde & PAGE_PRESENT) == 0) {
		if (!create)
			return NULL;

		uint32_t table_physical;
		bool backed_by_pmm;
		if (!legacy_alloc_page_table_frame(&table_physical, &backed_by_pmm))
			return NULL;

		legacy_recursive_page_directory[dir] =
			(table_physical & 0xFFFFF000u) | PAGE_PRESENT | PAGE_WRITABLE |
			(page_flags & PAGE_USER);
		legacy_table_backed_by_pmm[dir] = backed_by_pmm;
		paging_flush_tlb();

		volatile uint32_t* table = legacy_page_table_from_directory_index(dir);
		for (uint32_t i = 0; i < LEGACY_PTE_ENTRIES; i++)
			table[i] = 0;

		return table;
	}

	if ((page_flags & PAGE_USER) != 0 && (pde & PAGE_USER) == 0) {
		legacy_recursive_page_directory[dir] = pde | PAGE_USER | PAGE_WRITABLE;
		paging_flush_tlb();
	}

	return legacy_page_table_from_directory_index(dir);
}

static bool legacy_translate_address(uint32_t virtual_addr, uint32_t* physical_out) {
	const uint32_t dir = legacy_directory_index_of(virtual_addr);
	const uint32_t pde = legacy_recursive_page_directory[dir];

	if ((pde & PAGE_PRESENT) == 0)
		return false;

	if ((pde & 0x080u) != 0) {
		*physical_out = (pde & 0xFFC00000u) | (virtual_addr & 0x003FFFFFu);
		return true;
	}

	const volatile uint32_t* table = legacy_page_table_from_directory_index(dir);
	const uint32_t pte = table[legacy_table_index_of(virtual_addr)];
	if ((pte & PAGE_PRESENT) == 0)
		return false;

	*physical_out = (pte & 0xFFFFF000u) | (virtual_addr & 0xFFFu);
	return true;
}

static bool legacy_is_page_table_empty(uint32_t directory_index) {
	const volatile uint32_t* table = legacy_page_table_from_directory_index(directory_index);

	for (uint32_t i = 0; i < LEGACY_PTE_ENTRIES; i++) {
		if ((table[i] & PAGE_PRESENT) != 0)
			return false;
	}

	return true;
}

static bool legacy_map_page(void* virtual_addr, uint32_t physical_addr, uint32_t flags) {
	const uint32_t address = (uint32_t) (uintptr_t) virtual_addr;

	if ((address & 0xFFFu) != 0 || (physical_addr & 0xFFFu) != 0)
		return false;

	if (legacy_directory_index_of(address) == LEGACY_RECURSIVE_PDE_INDEX)
		return false;

	volatile uint32_t* table = legacy_get_page_table(address, true, flags);
	if (table == NULL)
		return false;

	const uint32_t index = legacy_table_index_of(address);
	table[index] = (physical_addr & 0xFFFFF000u) | (flags & 0x0FFFu) | PAGE_PRESENT;
	paging_invalidate_page(virtual_addr);
	return true;
}

static void legacy_unmap_page(void* virtual_addr) {
	const uint32_t address = (uint32_t) (uintptr_t) virtual_addr;
	const uint32_t dir = legacy_directory_index_of(address);

	if (dir == LEGACY_RECURSIVE_PDE_INDEX)
		return;

	volatile uint32_t* table = legacy_get_page_table(address, false, 0);
	if (table == NULL)
		return;

	const uint32_t index = legacy_table_index_of(address);
	if ((table[index] & PAGE_PRESENT) == 0)
		return;

	table[index] = 0;
	paging_invalidate_page(virtual_addr);

	if (!legacy_table_backed_by_pmm[dir] || !pmm_is_initialized())
		return;

	if (!legacy_is_page_table_empty(dir))
		return;

	const uint32_t table_physical = legacy_recursive_page_directory[dir] & 0xFFFFF000u;
	legacy_recursive_page_directory[dir] = 0;
	legacy_table_backed_by_pmm[dir] = false;
	paging_flush_tlb();
	pmm_free_frame(table_physical);
}

static bool legacy_is_mapped(const void* virtual_addr) {
	const uint32_t address = (uint32_t) (uintptr_t) virtual_addr;
	const volatile uint32_t* table = legacy_get_page_table(address, false, 0);
	if (table == NULL)
		return false;

	return (table[legacy_table_index_of(address)] & PAGE_PRESENT) != 0;
}

static void legacy_protect_range(uint32_t start, uint32_t end) {
	if (end <= start)
		return;

	const uint32_t aligned_start = align_down_u32(start, PAGE_SIZE);
	const uint32_t aligned_end = align_up_u32(end, PAGE_SIZE);

	for (uint32_t address = aligned_start; address < aligned_end; address += PAGE_SIZE) {
		volatile uint32_t* table = legacy_get_page_table(address, false, 0);
		if (table == NULL)
			continue;

		const uint32_t index = legacy_table_index_of(address);
		if ((table[index] & PAGE_PRESENT) == 0)
			continue;

		table[index] &= ~PAGE_WRITABLE;
		paging_invalidate_page((void*) (uintptr_t) address);
	}
}

// -----------------------------------------------------------------------------
// PAE runtime paging
// -----------------------------------------------------------------------------

#define PAE_PDPT_ENTRIES               4u
#define PAE_PDE_ENTRIES                512u
#define PAE_PTE_ENTRIES                512u
#define PAE_PDPT_ENTRY_LOW             0u
#define PAE_PDPT_ENTRY_KERNEL          3u
#define PAE_DIRECT_MAP_PDE_COUNT       (KERNEL_DIRECT_MAP_LIMIT_PHYS / (PAE_PTE_ENTRIES * PAGE_SIZE))
#define PAE_DYNAMIC_PDE_START          ((KERNEL_DYNAMIC_BASE - KERNEL_VMA) / (PAE_PTE_ENTRIES * PAGE_SIZE))
#define PAE_DYNAMIC_PDE_END            ((KERNEL_DYNAMIC_LIMIT - KERNEL_VMA) / (PAE_PTE_ENTRIES * PAGE_SIZE))
#define PAE_DYNAMIC_PDE_COUNT          (PAE_DYNAMIC_PDE_END - PAE_DYNAMIC_PDE_START)

#define PAE_ENTRY_PRESENT              0x001ULL
#define PAE_ENTRY_WRITABLE             0x002ULL
#define PAE_ENTRY_USER                 0x004ULL
#define PAE_ENTRY_PAGE_SIZE            0x080ULL
#define PAE_ENTRY_ADDR_MASK            0x00000000FFFFF000ULL

static uint64_t pae_pdpt[PAE_PDPT_ENTRIES] __attribute__((aligned(32)));
static uint64_t pae_identity_page_directory[PAE_PDE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t pae_identity_page_tables[2][PAE_PTE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t pae_kernel_page_directory[PAE_PDE_ENTRIES] __attribute__((aligned(4096)));
static uint64_t pae_direct_map_page_tables[PAE_DIRECT_MAP_PDE_COUNT][PAE_PTE_ENTRIES]
	__attribute__((aligned(4096)));
static uint64_t pae_dynamic_page_tables[PAE_DYNAMIC_PDE_COUNT][PAE_PTE_ENTRIES]
	__attribute__((aligned(4096)));

static uint64_t pae_make_entry(uint32_t physical, uint64_t flags) {
	return ((uint64_t) physical & PAE_ENTRY_ADDR_MASK) | flags;
}

static uint32_t kernel_pointer_to_physical(const void* virtual_addr) {
	const uint32_t address = (uint32_t) (uintptr_t) virtual_addr;
	if (address < KERNEL_VMA)
		panic("kernel symbol %p is outside higher-half mapping", virtual_addr);

	return address - KERNEL_VMA;
}

static uint32_t pae_pdpt_index_of(uint32_t virtual_addr) {
	return virtual_addr >> 30;
}

static uint32_t pae_directory_index_of(uint32_t virtual_addr) {
	return (virtual_addr >> 21) & 0x1FFu;
}

static uint32_t pae_table_index_of(uint32_t virtual_addr) {
	return (virtual_addr >> 12) & 0x1FFu;
}

static bool pae_address_is_dynamic(uint32_t virtual_addr) {
	return virtual_addr >= KERNEL_DYNAMIC_BASE && virtual_addr < KERNEL_DYNAMIC_LIMIT;
}

static bool pae_directory_is_dynamic(uint32_t directory_index) {
	return directory_index >= PAE_DYNAMIC_PDE_START && directory_index < PAE_DYNAMIC_PDE_END;
}

static uint64_t* pae_page_table_for_directory(uint32_t directory_index) {
	if (directory_index < PAE_DIRECT_MAP_PDE_COUNT)
		return &pae_direct_map_page_tables[directory_index][0];

	if (pae_directory_is_dynamic(directory_index))
		return &pae_dynamic_page_tables[directory_index - PAE_DYNAMIC_PDE_START][0];

	return NULL;
}

static bool pae_is_page_table_empty(uint32_t directory_index) {
	uint64_t* table = pae_page_table_for_directory(directory_index);
	if (table == NULL)
		return true;

	for (uint32_t i = 0; i < PAE_PTE_ENTRIES; i++) {
		if ((table[i] & PAE_ENTRY_PRESENT) != 0)
			return false;
	}

	return true;
}

static void pae_prepare_bootstrap_tables(void) {
	for (uint32_t i = 0; i < PAE_PDPT_ENTRIES; i++)
		pae_pdpt[i] = 0;

	for (uint32_t pde = 0; pde < PAE_PDE_ENTRIES; pde++)
		pae_identity_page_directory[pde] = 0;

	for (uint32_t pde = 0; pde < PAE_PDE_ENTRIES; pde++)
		pae_kernel_page_directory[pde] = 0;

	for (uint32_t pde = 0; pde < PAE_DIRECT_MAP_PDE_COUNT; pde++) {
		for (uint32_t pte = 0; pte < PAE_PTE_ENTRIES; pte++)
			pae_direct_map_page_tables[pde][pte] = 0;
	}

	for (uint32_t pde = 0; pde < PAE_DYNAMIC_PDE_COUNT; pde++) {
		for (uint32_t pte = 0; pte < PAE_PTE_ENTRIES; pte++)
			pae_dynamic_page_tables[pde][pte] = 0;
	}

	for (uint32_t table = 0; table < 2; table++) {
		for (uint32_t entry = 0; entry < PAE_PTE_ENTRIES; entry++) {
			const uint32_t physical = (table * PAE_PTE_ENTRIES + entry) * PAGE_SIZE;
			pae_identity_page_tables[table][entry] =
				pae_make_entry(physical, PAE_ENTRY_PRESENT | PAE_ENTRY_WRITABLE);
		}

		pae_identity_page_directory[table] =
			pae_make_entry(kernel_pointer_to_physical(&pae_identity_page_tables[table][0]),
				PAE_ENTRY_PRESENT | PAE_ENTRY_WRITABLE);
	}

	for (uint32_t pde = 0; pde < PAE_DIRECT_MAP_PDE_COUNT; pde++) {
		uint64_t* table = &pae_direct_map_page_tables[pde][0];
		pae_kernel_page_directory[pde] =
			pae_make_entry(kernel_pointer_to_physical(table),
				PAE_ENTRY_PRESENT | PAE_ENTRY_WRITABLE);

		const uint32_t base_physical = pde * PAE_PTE_ENTRIES * PAGE_SIZE;
		for (uint32_t pte = 0; pte < PAE_PTE_ENTRIES; pte++) {
			const uint32_t physical = base_physical + pte * PAGE_SIZE;
			table[pte] =
				pae_make_entry(physical, PAE_ENTRY_PRESENT | PAE_ENTRY_WRITABLE);
		}
	}

	pae_pdpt[PAE_PDPT_ENTRY_LOW] =
		pae_make_entry(kernel_pointer_to_physical(&pae_identity_page_directory[0]),
			PAE_ENTRY_PRESENT | PAE_ENTRY_WRITABLE);
	pae_pdpt[PAE_PDPT_ENTRY_KERNEL] =
		pae_make_entry(kernel_pointer_to_physical(&pae_kernel_page_directory[0]),
			PAE_ENTRY_PRESENT | PAE_ENTRY_WRITABLE);
}

static void pae_activate_runtime(void) {
	const uint32_t pdpt_physical = kernel_pointer_to_physical(&pae_pdpt[0]);
	const uint32_t trampoline_virtual = (uint32_t) (uintptr_t) i386_pae_trampoline;
	if (trampoline_virtual < KERNEL_VMA)
		panic("PAE trampoline is outside higher-half mapping");
	const uint32_t trampoline_physical = trampoline_virtual - KERNEL_VMA;

	uint32_t high_stack;
	asm volatile("mov %%esp, %0" : "=r"(high_stack));

	if (high_stack < KERNEL_VMA)
		panic("unexpected stack outside higher-half during PAE switch");

	const uint32_t low_stack = high_stack - KERNEL_VMA;
	if (low_stack >= BOOTSTRAP_WINDOW_SIZE)
		panic("PAE switch stack is outside identity-mapped bootstrap window");

	if (trampoline_physical >= BOOTSTRAP_WINDOW_SIZE)
		panic("PAE trampoline is outside identity-mapped bootstrap window");

	typedef void (*pae_trampoline_fn_t)(uint32_t, uint32_t, uint32_t);
	pae_trampoline_fn_t trampoline = (pae_trampoline_fn_t) (uintptr_t) trampoline_physical;

	x86_cli();
	trampoline(pdpt_physical, low_stack, high_stack);
}

static bool pae_translate_address(uint32_t virtual_addr, uint32_t* physical_out) {
	const uint32_t pdpt_index = pae_pdpt_index_of(virtual_addr);
	const uint32_t directory_index = pae_directory_index_of(virtual_addr);
	const uint32_t table_index = pae_table_index_of(virtual_addr);
	const uint64_t pdpte = pae_pdpt[pdpt_index];

	if ((pdpte & PAE_ENTRY_PRESENT) == 0)
		return false;

	if (pdpt_index == PAE_PDPT_ENTRY_KERNEL) {
		const uint64_t pde = pae_kernel_page_directory[directory_index];
		if ((pde & PAE_ENTRY_PRESENT) == 0)
			return false;

		if ((pde & PAE_ENTRY_PAGE_SIZE) != 0) {
			*physical_out =
				(uint32_t) (pde & 0x00000000FFE00000ULL) | (virtual_addr & 0x001FFFFFu);
			return true;
		}

		uint64_t* table = pae_page_table_for_directory(directory_index);
		if (table == NULL)
			return false;

		const uint64_t pte = table[table_index];
		if ((pte & PAE_ENTRY_PRESENT) == 0)
			return false;

		*physical_out = (uint32_t) (pte & PAE_ENTRY_ADDR_MASK) | (virtual_addr & 0xFFFu);
		return true;
	}

	if (pdpt_index == PAE_PDPT_ENTRY_LOW) {
		if (directory_index >= 2)
			return false;

		const uint64_t pde = pae_identity_page_directory[directory_index];
		if ((pde & PAE_ENTRY_PRESENT) == 0)
			return false;

		const uint64_t pte = pae_identity_page_tables[directory_index][table_index];
		if ((pte & PAE_ENTRY_PRESENT) == 0)
			return false;

		*physical_out = (uint32_t) (pte & PAE_ENTRY_ADDR_MASK) | (virtual_addr & 0xFFFu);
		return true;
	}

	return false;
}

static bool pae_map_page(void* virtual_addr, uint32_t physical_addr, uint32_t flags) {
	const uint32_t address = (uint32_t) (uintptr_t) virtual_addr;
	const uint32_t directory_index = pae_directory_index_of(address);
	const uint32_t table_index = pae_table_index_of(address);

	if ((address & 0xFFFu) != 0 || (physical_addr & 0xFFFu) != 0)
		return false;

	if (!pae_address_is_dynamic(address) || pae_pdpt_index_of(address) != PAE_PDPT_ENTRY_KERNEL)
		return false;

	if (!pae_directory_is_dynamic(directory_index))
		return false;

	uint64_t* table = pae_page_table_for_directory(directory_index);
	if (table == NULL)
		return false;

	uint64_t pde = pae_kernel_page_directory[directory_index];
	if ((pde & PAE_ENTRY_PRESENT) == 0) {
		for (uint32_t i = 0; i < PAE_PTE_ENTRIES; i++)
			table[i] = 0;

		pae_kernel_page_directory[directory_index] =
			pae_make_entry(kernel_pointer_to_physical(table),
				PAE_ENTRY_PRESENT | PAE_ENTRY_WRITABLE |
				((flags & PAGE_USER) != 0 ? PAE_ENTRY_USER : 0));
	} else if ((flags & PAGE_USER) != 0 && (pde & PAE_ENTRY_USER) == 0) {
		pae_kernel_page_directory[directory_index] = pde | PAE_ENTRY_USER | PAE_ENTRY_WRITABLE;
	}

	table[table_index] =
		pae_make_entry(physical_addr, PAE_ENTRY_PRESENT | (flags & 0x0FFFu));
	paging_invalidate_page(virtual_addr);
	return true;
}

static void pae_unmap_page(void* virtual_addr) {
	const uint32_t address = (uint32_t) (uintptr_t) virtual_addr;

	if (!pae_address_is_dynamic(address) || pae_pdpt_index_of(address) != PAE_PDPT_ENTRY_KERNEL)
		return;

	const uint32_t directory_index = pae_directory_index_of(address);
	const uint32_t table_index = pae_table_index_of(address);
	if (!pae_directory_is_dynamic(directory_index))
		return;

	uint64_t* table = pae_page_table_for_directory(directory_index);
	if (table == NULL)
		return;

	if ((pae_kernel_page_directory[directory_index] & PAE_ENTRY_PRESENT) == 0)
		return;

	if ((table[table_index] & PAE_ENTRY_PRESENT) == 0)
		return;

	table[table_index] = 0;
	paging_invalidate_page(virtual_addr);

	if (!pae_is_page_table_empty(directory_index))
		return;

	pae_kernel_page_directory[directory_index] = 0;
}

static bool pae_is_mapped(const void* virtual_addr) {
	uint32_t physical;
	return pae_translate_address((uint32_t) (uintptr_t) virtual_addr, &physical);
}

static void pae_protect_range(uint32_t start, uint32_t end) {
	if (end <= start)
		return;

	const uint32_t aligned_start = align_down_u32(start, PAGE_SIZE);
	const uint32_t aligned_end = align_up_u32(end, PAGE_SIZE);

	for (uint32_t address = aligned_start; address < aligned_end; address += PAGE_SIZE) {
		if (pae_pdpt_index_of(address) != PAE_PDPT_ENTRY_KERNEL)
			continue;

		const uint32_t directory_index = pae_directory_index_of(address);
		const uint32_t table_index = pae_table_index_of(address);

		if ((pae_kernel_page_directory[directory_index] & PAE_ENTRY_PRESENT) == 0)
			continue;

		uint64_t* table = pae_page_table_for_directory(directory_index);
		if (table == NULL)
			continue;

		uint64_t pte = table[table_index];
		if ((pte & PAE_ENTRY_PRESENT) == 0)
			continue;

		table[table_index] = pte & ~PAE_ENTRY_WRITABLE;
		paging_invalidate_page((void*) (uintptr_t) address);
	}
}

static void pae_drop_identity_mapping(void) {
	pae_pdpt[PAE_PDPT_ENTRY_LOW] = 0;
	paging_flush_tlb();
}

// -----------------------------------------------------------------------------
// Common paging interface
// -----------------------------------------------------------------------------

static bool paging_translate_address(uint32_t virtual_addr, uint32_t* physical_out) {
	if (paging_mode == PAGING_MODE_PAE)
		return pae_translate_address(virtual_addr, physical_out);

	return legacy_translate_address(virtual_addr, physical_out);
}

static void paging_apply_kernel_protections(void) {
	const uint32_t text_start = (uint32_t) (uintptr_t) &_text_start;
	const uint32_t text_end = (uint32_t) (uintptr_t) &_text_end;
	const uint32_t rodata_start = (uint32_t) (uintptr_t) &_rodata_start;
	const uint32_t rodata_end = (uint32_t) (uintptr_t) &_rodata_end;

	if (paging_mode == PAGING_MODE_PAE) {
		pae_protect_range(text_start, text_end);
		pae_protect_range(rodata_start, rodata_end);
		return;
	}

	legacy_protect_range(text_start, text_end);
	legacy_protect_range(rodata_start, rodata_end);
}

void paging_init(uint32_t multiboot_info_addr) {
	(void) multiboot_info_addr;

	legacy_early_table_count =
		(uint32_t) (((uintptr_t) boot_page_table_pool_end - (uintptr_t) boot_page_table_pool) /
			PAGE_SIZE);
	legacy_next_early_table = 0;
	next_dynamic_virtual = KERNEL_DYNAMIC_BASE;

	for (uint32_t i = 0; i < LEGACY_PDE_ENTRIES; i++)
		legacy_table_backed_by_pmm[i] = false;

	pae_supported = cpu_has_pae();
	pae_ready = false;
	pae_active = false;
	paging_mode = PAGING_MODE_LEGACY;

	if (pae_supported) {
		pae_prepare_bootstrap_tables();
		pae_activate_runtime();
		paging_mode = PAGING_MODE_PAE;
		pae_active = true;
		pae_ready = true;
		pae_drop_identity_mapping();
	}

	paging_apply_kernel_protections();
}

bool paging_pae_supported(void) {
	return pae_supported;
}

bool paging_pae_ready(void) {
	return pae_ready;
}

const char* paging_mode_name(void) {
	if (pae_active)
		return "PAE 32-bit paging";

	return "legacy 32-bit paging";
}

void* paging_phys_to_virt(uint32_t physical) {
	if (physical >= KERNEL_DIRECT_MAP_LIMIT_PHYS)
		panic("physical address 0x%x exceeds direct-map aperture", physical);

	const uint32_t virtual_addr = KERNEL_VMA + physical;
	const uint32_t virtual_page = align_down_u32(virtual_addr, PAGE_SIZE);
	const uint32_t physical_page = align_down_u32(physical, PAGE_SIZE);

	if (paging_mode == PAGING_MODE_LEGACY && !legacy_is_mapped((const void*) (uintptr_t) virtual_page)) {
		if (!legacy_map_page((void*) (uintptr_t) virtual_page, physical_page, PAGE_WRITABLE))
			panic("failed to map physical page 0x%x into direct map", physical_page);
	}

	if (paging_mode == PAGING_MODE_PAE && !pae_is_mapped((const void*) (uintptr_t) virtual_page))
		panic("PAE direct map is missing virtual page %p", (void*) (uintptr_t) virtual_page);

	return (void*) (uintptr_t) virtual_addr;
}

uint32_t paging_virt_to_phys(const void* virtual_addr) {
	uint32_t physical;
	if (!paging_translate_address((uint32_t) (uintptr_t) virtual_addr, &physical))
		panic("paging_virt_to_phys on unmapped address %p", virtual_addr);

	return physical;
}

uint32_t paging_window_end_phys(void) {
	return BOOTSTRAP_WINDOW_SIZE;
}

bool paging_map_page(void* virtual_addr, uint32_t physical_addr, uint32_t flags) {
	if (paging_mode == PAGING_MODE_PAE)
		return pae_map_page(virtual_addr, physical_addr, flags);

	return legacy_map_page(virtual_addr, physical_addr, flags);
}

void paging_unmap_page(void* virtual_addr) {
	if (paging_mode == PAGING_MODE_PAE) {
		pae_unmap_page(virtual_addr);
		return;
	}

	legacy_unmap_page(virtual_addr);
}

bool paging_is_mapped(const void* virtual_addr) {
	if (paging_mode == PAGING_MODE_PAE)
		return pae_is_mapped(virtual_addr);

	return legacy_is_mapped(virtual_addr);
}

void* paging_alloc_pages(uint32_t page_count) {
	if (page_count == 0 || !pmm_is_initialized())
		return NULL;

	if (next_dynamic_virtual >= KERNEL_DYNAMIC_LIMIT)
		return NULL;

	const uint32_t available_pages = (KERNEL_DYNAMIC_LIMIT - next_dynamic_virtual) / PAGE_SIZE;
	if (page_count > available_pages)
		return NULL;

	const uint32_t base_virtual = next_dynamic_virtual;
	uint32_t mapped_pages = 0;

	for (uint32_t i = 0; i < page_count; i++) {
		uint32_t frame;
		if (!pmm_alloc_frame(&frame))
			goto rollback;

		const uint32_t virtual_page = base_virtual + i * PAGE_SIZE;
		if (!paging_map_page((void*) (uintptr_t) virtual_page, frame, PAGE_WRITABLE)) {
			pmm_free_frame(frame);
			goto rollback;
		}

		mapped_pages++;
	}

	next_dynamic_virtual += page_count * PAGE_SIZE;
	return (void*) (uintptr_t) base_virtual;

rollback:
	while (mapped_pages > 0) {
		mapped_pages--;
		const uint32_t virtual_page = base_virtual + mapped_pages * PAGE_SIZE;
		uint32_t frame;
		if (paging_translate_address(virtual_page, &frame)) {
			pmm_free_frame(frame);
			paging_unmap_page((void*) (uintptr_t) virtual_page);
		}
	}

	return NULL;
}

void paging_free_pages(void* base, uint32_t page_count) {
	const uint32_t base_virtual = (uint32_t) (uintptr_t) base;

	for (uint32_t i = 0; i < page_count; i++) {
		const uint32_t virtual_page = base_virtual + i * PAGE_SIZE;
		uint32_t frame;
		if (!paging_translate_address(virtual_page, &frame))
			continue;

		pmm_free_frame(frame);
		paging_unmap_page((void*) (uintptr_t) virtual_page);
	}
}
