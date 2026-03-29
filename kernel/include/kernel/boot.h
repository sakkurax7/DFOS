#ifndef KERNEL_BOOT_H
#define KERNEL_BOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MULTIBOOT_INFO_MEMORY        (1u << 0)
#define MULTIBOOT_INFO_BOOT_DEVICE   (1u << 1)
#define MULTIBOOT_INFO_CMDLINE       (1u << 2)
#define MULTIBOOT_INFO_MODULES       (1u << 3)
#define MULTIBOOT_INFO_AOUT_SYMS     (1u << 4)
#define MULTIBOOT_INFO_ELF_SHDR      (1u << 5)
#define MULTIBOOT_INFO_MEM_MAP       (1u << 6)
#define MULTIBOOT_INFO_DRIVES        (1u << 7)
#define MULTIBOOT_INFO_CONFIG_TABLE  (1u << 8)
#define MULTIBOOT_INFO_BOOT_LOADER   (1u << 9)
#define MULTIBOOT_INFO_APM_TABLE     (1u << 10)
#define MULTIBOOT_INFO_VBE_INFO      (1u << 11)
#define MULTIBOOT_INFO_FRAMEBUFFER   (1u << 12)

#define MULTIBOOT_MEMORY_AVAILABLE   1u

typedef struct multiboot_info {
	uint32_t flags;
	uint32_t mem_lower;
	uint32_t mem_upper;
	uint32_t boot_device;
	uint32_t cmdline;
	uint32_t mods_count;
	uint32_t mods_addr;
	uint32_t syms[4];
	uint32_t mmap_length;
	uint32_t mmap_addr;
	uint32_t drives_length;
	uint32_t drives_addr;
	uint32_t config_table;
	uint32_t boot_loader_name;
	uint32_t apm_table;
	uint32_t vbe_control_info;
	uint32_t vbe_mode_info;
	uint16_t vbe_mode;
	uint16_t vbe_interface_seg;
	uint16_t vbe_interface_off;
	uint16_t vbe_interface_len;
} __attribute__((packed)) multiboot_info_t;

typedef struct multiboot_memory_map {
	uint32_t size;
	uint64_t addr;
	uint64_t len;
	uint32_t type;
} __attribute__((packed)) multiboot_memory_map_t;

typedef struct multiboot_module {
	uint32_t mod_start;
	uint32_t mod_end;
	uint32_t string;
	uint32_t reserved;
} __attribute__((packed)) multiboot_module_t;

#endif
