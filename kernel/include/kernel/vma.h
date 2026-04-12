#ifndef KERNEL_VMA_H
#define KERNEL_VMA_H

#include <stdbool.h>
#include <stdint.h>

typedef struct vma_tree {
	void* root;
	uint32_t node_count;
} vma_tree_t;

void vma_tree_init(vma_tree_t* tree);
void vma_tree_clear(vma_tree_t* tree);
bool vma_tree_insert(vma_tree_t* tree, uint32_t start, uint32_t end, uint32_t flags);
bool vma_tree_remove(vma_tree_t* tree, uint32_t start, uint32_t end);
bool vma_tree_find(const vma_tree_t* tree, uint32_t address,
	uint32_t* start_out, uint32_t* end_out, uint32_t* flags_out);
bool vma_tree_first(const vma_tree_t* tree,
	uint32_t* start_out, uint32_t* end_out, uint32_t* flags_out);
bool vma_tree_find_gap(const vma_tree_t* tree, uint32_t min_addr, uint32_t max_addr,
	uint32_t length, uint32_t alignment, uint32_t* start_out);
uint32_t vma_tree_node_count(const vma_tree_t* tree);

#endif
