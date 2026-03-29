#ifndef KERNEL_VFS_H
#define KERNEL_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct vfs_node {
	const char* name;
	const uint8_t* data;
	size_t size;
} vfs_node_t;

void vfs_init(uint32_t multiboot_info_addr);
uint32_t vfs_file_count(void);
bool vfs_get_file(uint32_t index, vfs_node_t* node);
bool vfs_open(const char* path, vfs_node_t* node);
size_t vfs_read(const vfs_node_t* node, size_t offset, void* buffer, size_t size);

#endif
