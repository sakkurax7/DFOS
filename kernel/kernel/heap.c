#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/heap.h>
#include <kernel/paging.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>

typedef struct heap_block {
	size_t size;
	int free;
	struct heap_block* next;
} heap_block_t;

static heap_block_t* heap_head;
static void* heap_begin;
static void* heap_limit;

static size_t align_up_size(size_t value, size_t alignment) {
	return (value + alignment - 1) & ~(alignment - 1);
}

static void coalesce(void) {
	for (heap_block_t* block = heap_head; block != NULL && block->next != NULL; ) {
		if (block->free && block->next->free) {
			block->size += sizeof(heap_block_t) + block->next->size;
			block->next = block->next->next;
			continue;
		}
		block = block->next;
	}
}

void heap_init(void) {
	const uint32_t heap_phys_start = align_up_size(pmm_bitmap_end_phys(), 16);
	const uint32_t heap_phys_end = paging_window_end_phys();

	if (heap_phys_end <= heap_phys_start + sizeof(heap_block_t))
		panic("bootstrap heap window exhausted");

	heap_begin = paging_phys_to_virt(heap_phys_start);
	heap_limit = paging_phys_to_virt(heap_phys_end);
	heap_head = (heap_block_t*) heap_begin;
	heap_head->size = heap_phys_end - heap_phys_start - sizeof(heap_block_t);
	heap_head->free = 1;
	heap_head->next = NULL;

	pmm_reserve_range(heap_phys_start, heap_phys_end - heap_phys_start);
}

void* kmalloc(size_t size) {
	const size_t aligned_size = align_up_size(size, 8);

	for (heap_block_t* block = heap_head; block != NULL; block = block->next) {
		if (!block->free || block->size < aligned_size)
			continue;

		if (block->size >= aligned_size + sizeof(heap_block_t) + 16) {
			heap_block_t* split =
				(heap_block_t*) ((uint8_t*) (block + 1) + aligned_size);
			split->size = block->size - aligned_size - sizeof(heap_block_t);
			split->free = 1;
			split->next = block->next;
			block->next = split;
			block->size = aligned_size;
		}

		block->free = 0;
		return block + 1;
	}

	panic("kmalloc(%u) exhausted the early heap", (uint32_t) size);
}

void* kzalloc(size_t size) {
	void* ptr = kmalloc(size);
	memset(ptr, 0, size);
	return ptr;
}

void kfree(void* ptr) {
	if (ptr == NULL)
		return;

	heap_block_t* block = ((heap_block_t*) ptr) - 1;
	block->free = 1;
	coalesce();
}

void* heap_start(void) {
	return heap_begin;
}

void* heap_end(void) {
	return heap_limit;
}
