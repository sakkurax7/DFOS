#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/bootinfo.h>
#include <kernel/paging.h>
#include <kernel/vfs.h>

#define VFS_MAX_FILES 32
#define TAR_BLOCK_SIZE 512u

typedef struct tar_header {
	char name[100];
	char mode[8];
	char uid[8];
	char gid[8];
	char size[12];
	char mtime[12];
	char checksum[8];
	char typeflag;
	char linkname[100];
	char magic[6];
	char version[2];
	char uname[32];
	char gname[32];
	char devmajor[8];
	char devminor[8];
	char prefix[155];
} tar_header_t;

static vfs_node_t files[VFS_MAX_FILES];
static uint32_t file_count;

static uint32_t tar_parse_octal(const char* text, size_t length) {
	uint32_t value = 0;

	for (size_t i = 0; i < length && text[i] != '\0'; i++) {
		if (text[i] < '0' || text[i] > '7')
			continue;
		value = (value << 3) + (uint32_t) (text[i] - '0');
	}

	return value;
}

static bool tar_block_is_zero(const uint8_t* block) {
	for (size_t i = 0; i < TAR_BLOCK_SIZE; i++) {
		if (block[i] != 0)
			return false;
	}
	return true;
}

static const char* normalize_name(const char* raw_name) {
	if (raw_name[0] == '.' && raw_name[1] == '/')
		return raw_name + 2;
	return raw_name;
}

static void vfs_load_initrd_module(uint32_t module_start, uint32_t module_end) {
	const uint8_t* cursor = (const uint8_t*) paging_phys_to_virt(module_start);
	const uint8_t* limit = (const uint8_t*) paging_phys_to_virt(module_end);

	// The initrd is currently a plain tar archive carried as the first Multiboot module.
	while (cursor + TAR_BLOCK_SIZE <= limit && !tar_block_is_zero(cursor)) {
		const tar_header_t* header = (const tar_header_t*) cursor;
		const uint32_t size = tar_parse_octal(header->size, sizeof(header->size));
		const uint32_t blocks = (size + TAR_BLOCK_SIZE - 1u) / TAR_BLOCK_SIZE;
		const uint8_t* data = cursor + TAR_BLOCK_SIZE;

		if (file_count < VFS_MAX_FILES && header->typeflag != '5') {
			// File contents remain in-place inside the module; the VFS stores pointers into the tar image.
			files[file_count].name = normalize_name(header->name);
			files[file_count].data = data;
			files[file_count].size = size;
			file_count++;
		}

		cursor += TAR_BLOCK_SIZE + blocks * TAR_BLOCK_SIZE;
	}
}

void vfs_init(void) {
	file_count = 0;

	if (bootinfo_module_count() == 0)
		return;

	// DFOS currently treats the first boot module as the system initrd by convention.
	bootinfo_module_t module;
	if (!bootinfo_module_at(0, &module))
		return;

	vfs_load_initrd_module(module.start, module.end);
}

uint32_t vfs_file_count(void) {
	return file_count;
}

bool vfs_get_file(uint32_t index, vfs_node_t* node) {
	if (index >= file_count)
		return false;

	*node = files[index];
	return true;
}

bool vfs_open(const char* path, vfs_node_t* node) {
	for (uint32_t i = 0; i < file_count; i++) {
		if (strcmp(files[i].name, path) == 0) {
			*node = files[i];
			return true;
		}
	}

	return false;
}

size_t vfs_read(const vfs_node_t* node, size_t offset, void* buffer, size_t size) {
	if (offset >= node->size)
		return 0;

	size_t remaining = node->size - offset;
	if (size > remaining)
		size = remaining;

	memcpy(buffer, node->data + offset, size);
	return size;
}
