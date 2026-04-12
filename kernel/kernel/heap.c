#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/heap.h>
#include <kernel/paging.h>
#include <kernel/panic.h>

#define KERNEL_DYNAMIC_BASE  0xF0000000u
#define KERNEL_DYNAMIC_LIMIT 0xFFC00000u

#define SLAB_CACHE_COUNT      9u
#define HEAP_MAX_SLABS        256u

#define HEAP_SLAB_MAGIC       0x534C4142u
#define HEAP_LARGE_MAGIC      0x4C415247u

typedef struct slab slab_t;

typedef struct slab_object_header {
	uint32_t magic;
	slab_t* slab;
	struct slab_object_header* next;
	uint32_t reserved;
} slab_object_header_t;

typedef struct large_allocation_header {
	uint32_t magic;
	uint32_t page_count;
} large_allocation_header_t;

typedef struct slab_cache {
	size_t object_size;
	size_t slot_size;
	slab_t* partial;
	slab_t* full;
	slab_t* empty;
} slab_cache_t;

struct slab {
	slab_cache_t* cache;
	void* page_base;
	slab_object_header_t* free_list;
	uint16_t in_use;
	uint16_t capacity;
	slab_t* next;
};

static slab_cache_t slab_caches[SLAB_CACHE_COUNT];
static slab_t slab_metadata_pool[HEAP_MAX_SLABS];
static slab_t* slab_metadata_free_list;
static bool heap_ready;
static void* heap_begin;
static void* heap_limit;

static size_t align_up_size(size_t value, size_t alignment) {
	return (value + alignment - 1u) & ~(alignment - 1u);
}

static void slab_list_push(slab_t** head, slab_t* slab) {
	slab->next = *head;
	*head = slab;
}

static void slab_list_remove(slab_t** head, slab_t* slab) {
	slab_t** cursor = head;

	while (*cursor != NULL) {
		if (*cursor == slab) {
			*cursor = slab->next;
			slab->next = NULL;
			return;
		}
		cursor = &(*cursor)->next;
	}
}

static slab_t* slab_metadata_alloc(void) {
	if (slab_metadata_free_list == NULL)
		return NULL;

	slab_t* slab = slab_metadata_free_list;
	slab_metadata_free_list = slab_metadata_free_list->next;
	slab->next = NULL;
	return slab;
}

static void slab_metadata_free(slab_t* slab) {
	slab->next = slab_metadata_free_list;
	slab_metadata_free_list = slab;
}

static slab_cache_t* slab_cache_for_size(size_t size) {
	for (uint32_t i = 0; i < SLAB_CACHE_COUNT; i++) {
		if (size <= slab_caches[i].object_size)
			return &slab_caches[i];
	}

	return NULL;
}

static slab_t* slab_create(slab_cache_t* cache) {
	void* page = paging_alloc_pages(1);
	if (page == NULL)
		return NULL;

	slab_t* slab = slab_metadata_alloc();
	if (slab == NULL) {
		paging_free_pages(page, 1);
		return NULL;
	}

	const uint32_t capacity = (uint32_t) (PAGE_SIZE / cache->slot_size);
	if (capacity == 0) {
		slab_metadata_free(slab);
		paging_free_pages(page, 1);
		return NULL;
	}

	slab->cache = cache;
	slab->page_base = page;
	slab->free_list = NULL;
	slab->in_use = 0;
	slab->capacity = (uint16_t) capacity;
	slab->next = NULL;

	uint8_t* cursor = (uint8_t*) page;
	for (uint32_t i = 0; i < capacity; i++) {
		slab_object_header_t* object =
			(slab_object_header_t*) (cursor + i * cache->slot_size);
		object->magic = HEAP_SLAB_MAGIC;
		object->slab = slab;
		object->reserved = 0;
		object->next = slab->free_list;
		slab->free_list = object;
	}

	return slab;
}

static void slab_destroy(slab_t* slab) {
	paging_free_pages(slab->page_base, 1);
	slab_metadata_free(slab);
}

static void* slab_alloc_from_cache(slab_cache_t* cache) {
	slab_t* slab = cache->partial;

	if (slab == NULL) {
		if (cache->empty != NULL) {
			slab = cache->empty;
			slab_list_remove(&cache->empty, slab);
		} else {
			slab = slab_create(cache);
			if (slab == NULL)
				return NULL;
		}

		slab_list_push(&cache->partial, slab);
	}

	slab_object_header_t* object = slab->free_list;
	if (object == NULL)
		return NULL;

	slab->free_list = object->next;
	slab->in_use++;
	object->next = NULL;

	if (slab->free_list == NULL) {
		slab_list_remove(&cache->partial, slab);
		slab_list_push(&cache->full, slab);
	}

	return object + 1;
}

static bool slab_free_pointer(void* ptr) {
	slab_object_header_t* object = ((slab_object_header_t*) ptr) - 1;
	if (object->magic != HEAP_SLAB_MAGIC || object->slab == NULL)
		return false;

	slab_t* slab = object->slab;
	slab_cache_t* cache = slab->cache;
	const bool was_full = slab->free_list == NULL;

	object->next = slab->free_list;
	slab->free_list = object;

	if (slab->in_use == 0)
		panic("double free detected for slab object %p", ptr);
	slab->in_use--;

	if (slab->in_use == 0) {
		if (was_full)
			slab_list_remove(&cache->full, slab);
		else
			slab_list_remove(&cache->partial, slab);

		if (cache->empty == NULL) {
			slab_list_push(&cache->empty, slab);
		} else {
			slab_destroy(slab);
		}

		return true;
	}

	if (was_full) {
		slab_list_remove(&cache->full, slab);
		slab_list_push(&cache->partial, slab);
	}

	return true;
}

static void* allocate_large(size_t size) {
	const size_t total = size + sizeof(large_allocation_header_t);
	const uint32_t page_count = (uint32_t) align_up_size(total, PAGE_SIZE) / PAGE_SIZE;
	void* allocation = paging_alloc_pages(page_count);
	if (allocation == NULL)
		return NULL;

	large_allocation_header_t* header = (large_allocation_header_t*) allocation;
	header->magic = HEAP_LARGE_MAGIC;
	header->page_count = page_count;
	return header + 1;
}

void heap_init(void) {
	static const size_t object_sizes[SLAB_CACHE_COUNT] = {
		8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u
	};

	for (uint32_t i = 0; i < HEAP_MAX_SLABS - 1u; i++)
		slab_metadata_pool[i].next = &slab_metadata_pool[i + 1u];
	slab_metadata_pool[HEAP_MAX_SLABS - 1u].next = NULL;
	slab_metadata_free_list = &slab_metadata_pool[0];

	for (uint32_t i = 0; i < SLAB_CACHE_COUNT; i++) {
		slab_caches[i].object_size = object_sizes[i];
		slab_caches[i].slot_size =
			align_up_size(sizeof(slab_object_header_t) + object_sizes[i], 8u);
		slab_caches[i].partial = NULL;
		slab_caches[i].full = NULL;
		slab_caches[i].empty = NULL;
	}

	heap_begin = (void*) (uintptr_t) KERNEL_DYNAMIC_BASE;
	heap_limit = (void*) (uintptr_t) KERNEL_DYNAMIC_LIMIT;
	heap_ready = true;
}

void* kmalloc(size_t size) {
	if (!heap_ready)
		panic("kmalloc before heap_init");

	if (size == 0)
		size = 1;

	slab_cache_t* cache = slab_cache_for_size(size);
	void* allocation = cache != NULL ? slab_alloc_from_cache(cache) : allocate_large(size);

	if (allocation == NULL)
		panic("kmalloc(%u) exhausted heap resources", (uint32_t) size);

	return allocation;
}

void* kzalloc(size_t size) {
	void* ptr = kmalloc(size);
	memset(ptr, 0, size);
	return ptr;
}

void kfree(void* ptr) {
	if (ptr == NULL)
		return;

	if (slab_free_pointer(ptr))
		return;

	large_allocation_header_t* header = ((large_allocation_header_t*) ptr) - 1;
	if (header->magic != HEAP_LARGE_MAGIC)
		panic("kfree on unknown pointer %p", ptr);

	paging_free_pages((void*) header, header->page_count);
}

void* heap_start(void) {
	return heap_begin;
}

void* heap_end(void) {
	return heap_limit;
}
