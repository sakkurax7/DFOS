#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/boot.h>
#include <kernel/bootinfo.h>
#include <kernel/paging.h>

static bootinfo_loader_kind_t loader_kind;
static uint32_t info_phys;
static const multiboot_info_t* multiboot_info;

static bool multiboot_map_bounds(uint32_t* start_phys_out, uint32_t* end_phys_out) {
	if (multiboot_info == NULL)
		return false;

	if ((multiboot_info->flags & MULTIBOOT_INFO_MEM_MAP) == 0)
		return false;

	const uint32_t start = multiboot_info->mmap_addr;
	const uint32_t length = multiboot_info->mmap_length;
	const uint32_t end = start + length;

	if (end < start)
		return false;

	*start_phys_out = start;
	*end_phys_out = end;
	return true;
}

static bool multiboot_decode_memory_region(uint32_t cursor_phys,
		bootinfo_memory_region_t* region_out, uint32_t* next_cursor_phys_out) {
	uint32_t map_start_phys;
	uint32_t map_end_phys;
	if (!multiboot_map_bounds(&map_start_phys, &map_end_phys))
		return false;

	if (cursor_phys < map_start_phys || cursor_phys >= map_end_phys)
		return false;

	const multiboot_memory_map_t* entry =
		(const multiboot_memory_map_t*) paging_phys_to_virt(cursor_phys);
	const uint32_t payload_size = entry->size;
	const uint32_t entry_size = payload_size + sizeof(entry->size);

	// GRUB memory-map entries are size-prefixed records with at least addr/len/type payload.
	if (payload_size < (sizeof(multiboot_memory_map_t) - sizeof(entry->size)))
		return false;

	if (entry_size < sizeof(entry->size))
		return false;

	const uint32_t next_cursor = cursor_phys + entry_size;
	if (next_cursor <= cursor_phys || next_cursor > map_end_phys)
		return false;

	region_out->base = entry->addr;
	region_out->length = entry->len;
	region_out->type = entry->type;
	*next_cursor_phys_out = next_cursor;
	return true;
}

bool bootinfo_init(uint32_t boot_magic, uint32_t boot_info_phys) {
	loader_kind = BOOTINFO_LOADER_UNKNOWN;
	info_phys = 0;
	multiboot_info = NULL;

	if (boot_magic != MULTIBOOT_BOOTLOADER_MAGIC)
		return false;

	info_phys = boot_info_phys;
	multiboot_info = (const multiboot_info_t*) paging_phys_to_virt(boot_info_phys);
	loader_kind = BOOTINFO_LOADER_MULTIBOOT1;
	return true;
}

bootinfo_loader_kind_t bootinfo_loader_kind(void) {
	return loader_kind;
}

uint32_t bootinfo_info_phys(void) {
	return info_phys;
}

uint32_t bootinfo_info_size_hint(void) {
	if (loader_kind != BOOTINFO_LOADER_MULTIBOOT1)
		return 0;

	return sizeof(multiboot_info_t);
}

bool bootinfo_has_memory_map(void) {
	if (loader_kind != BOOTINFO_LOADER_MULTIBOOT1 || multiboot_info == NULL)
		return false;

	return (multiboot_info->flags & MULTIBOOT_INFO_MEM_MAP) != 0 &&
		multiboot_info->mmap_length > 0;
}

uint32_t bootinfo_memory_map_phys(void) {
	if (!bootinfo_has_memory_map())
		return 0;

	return multiboot_info->mmap_addr;
}

uint32_t bootinfo_memory_map_size(void) {
	if (!bootinfo_has_memory_map())
		return 0;

	return multiboot_info->mmap_length;
}

bool bootinfo_memory_begin(bootinfo_memory_iterator_t* iterator,
		bootinfo_memory_region_t* region_out) {
	uint32_t start_phys;
	uint32_t end_phys;
	if (!multiboot_map_bounds(&start_phys, &end_phys))
		return false;

	(void) end_phys;
	iterator->cursor_phys = start_phys;
	return bootinfo_memory_next(iterator, region_out);
}

bool bootinfo_memory_next(bootinfo_memory_iterator_t* iterator,
		bootinfo_memory_region_t* region_out) {
	if (iterator->cursor_phys == 0)
		return false;

	uint32_t map_start_phys;
	uint32_t map_end_phys;
	if (!multiboot_map_bounds(&map_start_phys, &map_end_phys))
		return false;

	if (iterator->cursor_phys < map_start_phys || iterator->cursor_phys >= map_end_phys)
		return false;

	uint32_t next_cursor;
	if (!multiboot_decode_memory_region(iterator->cursor_phys, region_out, &next_cursor))
		return false;

	iterator->cursor_phys = next_cursor;
	if (iterator->cursor_phys >= map_end_phys)
		iterator->cursor_phys = 0;
	return true;
}

bool bootinfo_has_memory_kib_info(void) {
	if (loader_kind != BOOTINFO_LOADER_MULTIBOOT1 || multiboot_info == NULL)
		return false;

	return (multiboot_info->flags & MULTIBOOT_INFO_MEMORY) != 0;
}

uint32_t bootinfo_lower_memory_kib(void) {
	if (!bootinfo_has_memory_kib_info())
		return 0;

	return multiboot_info->mem_lower;
}

uint32_t bootinfo_upper_memory_kib(void) {
	if (!bootinfo_has_memory_kib_info())
		return 0;

	return multiboot_info->mem_upper;
}

uint32_t bootinfo_module_count(void) {
	if (loader_kind != BOOTINFO_LOADER_MULTIBOOT1 || multiboot_info == NULL)
		return 0;

	if ((multiboot_info->flags & MULTIBOOT_INFO_MODULES) == 0)
		return 0;

	return multiboot_info->mods_count;
}

bool bootinfo_module_at(uint32_t index, bootinfo_module_t* module_out) {
	const uint32_t count = bootinfo_module_count();
	if (index >= count)
		return false;

	const multiboot_module_t* modules =
		(const multiboot_module_t*) paging_phys_to_virt(multiboot_info->mods_addr);
	module_out->start = modules[index].mod_start;
	module_out->end = modules[index].mod_end;
	module_out->string =
		modules[index].string == 0 ? NULL : (const char*) paging_phys_to_virt(modules[index].string);
	return true;
}

uint32_t bootinfo_module_table_phys(void) {
	if (bootinfo_module_count() == 0)
		return 0;

	return multiboot_info->mods_addr;
}

uint32_t bootinfo_module_table_size(void) {
	const uint32_t count = bootinfo_module_count();
	if (count == 0)
		return 0;

	return count * sizeof(multiboot_module_t);
}
