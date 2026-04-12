#ifndef KERNEL_BOOTINFO_H
#define KERNEL_BOOTINFO_H

#include <stdbool.h>
#include <stdint.h>

typedef enum bootinfo_loader_kind {
	BOOTINFO_LOADER_UNKNOWN = 0,
	BOOTINFO_LOADER_MULTIBOOT1
} bootinfo_loader_kind_t;

typedef struct bootinfo_memory_region {
	uint64_t base;
	uint64_t length;
	uint32_t type;
} bootinfo_memory_region_t;

typedef struct bootinfo_memory_iterator {
	uint32_t cursor_phys;
} bootinfo_memory_iterator_t;

typedef struct bootinfo_module {
	uint32_t start;
	uint32_t end;
	const char* string;
} bootinfo_module_t;

bool bootinfo_init(uint32_t boot_magic, uint32_t boot_info_phys);
bootinfo_loader_kind_t bootinfo_loader_kind(void);
uint32_t bootinfo_info_phys(void);
uint32_t bootinfo_info_size_hint(void);
bool bootinfo_has_memory_map(void);
uint32_t bootinfo_memory_map_phys(void);
uint32_t bootinfo_memory_map_size(void);
bool bootinfo_memory_begin(bootinfo_memory_iterator_t* iterator,
	bootinfo_memory_region_t* region_out);
bool bootinfo_memory_next(bootinfo_memory_iterator_t* iterator,
	bootinfo_memory_region_t* region_out);
bool bootinfo_has_memory_kib_info(void);
uint32_t bootinfo_lower_memory_kib(void);
uint32_t bootinfo_upper_memory_kib(void);
uint32_t bootinfo_module_count(void);
bool bootinfo_module_at(uint32_t index, bootinfo_module_t* module_out);
uint32_t bootinfo_module_table_phys(void);
uint32_t bootinfo_module_table_size(void);

#endif
