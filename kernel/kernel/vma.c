#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/vma.h>

#define VMA_MAX_NODES 2048u

typedef struct vma_node {
	uint32_t start;
	uint32_t end;
	uint32_t flags;
	int8_t height;
	struct vma_node* left;
	struct vma_node* right;
} vma_node_t;

typedef struct gap_search_state {
	uint32_t cursor;
	uint32_t max_addr;
	uint32_t length;
	uint32_t alignment;
	uint32_t result;
	bool found;
} gap_search_state_t;

static vma_node_t node_pool[VMA_MAX_NODES];
static vma_node_t* free_nodes;
static bool pool_initialized;

static int max_i32(int a, int b) {
	return a > b ? a : b;
}

static uint32_t align_up_u32(uint32_t value, uint32_t alignment) {
	if (alignment <= 1)
		return value;

	const uint64_t aligned =
		((uint64_t) value + (uint64_t) alignment - 1u) & ~((uint64_t) alignment - 1u);
	if (aligned > 0xFFFFFFFFu)
		return 0;
	return (uint32_t) aligned;
}

static bool add_fits_limit(uint32_t start, uint32_t length, uint32_t limit) {
	const uint64_t end = (uint64_t) start + (uint64_t) length;
	return end <= (uint64_t) limit;
}

static void node_pool_init(void) {
	if (pool_initialized)
		return;

	free_nodes = NULL;
	for (uint32_t i = 0; i < VMA_MAX_NODES; i++) {
		node_pool[i].left = free_nodes;
		free_nodes = &node_pool[i];
	}

	pool_initialized = true;
}

static vma_node_t* node_alloc(uint32_t start, uint32_t end, uint32_t flags) {
	node_pool_init();
	if (free_nodes == NULL)
		return NULL;

	vma_node_t* node = free_nodes;
	free_nodes = free_nodes->left;
	node->start = start;
	node->end = end;
	node->flags = flags;
	node->height = 1;
	node->left = NULL;
	node->right = NULL;
	return node;
}

static void node_free(vma_node_t* node) {
	node->left = free_nodes;
	node->right = NULL;
	free_nodes = node;
}

static int node_height(const vma_node_t* node) {
	return node == NULL ? 0 : node->height;
}

static void node_refresh_height(vma_node_t* node) {
	node->height = (int8_t) (1 + max_i32(node_height(node->left), node_height(node->right)));
}

static int node_balance_factor(const vma_node_t* node) {
	return node_height(node->left) - node_height(node->right);
}

static vma_node_t* rotate_right(vma_node_t* node) {
	vma_node_t* pivot = node->left;
	node->left = pivot->right;
	pivot->right = node;
	node_refresh_height(node);
	node_refresh_height(pivot);
	return pivot;
}

static vma_node_t* rotate_left(vma_node_t* node) {
	vma_node_t* pivot = node->right;
	node->right = pivot->left;
	pivot->left = node;
	node_refresh_height(node);
	node_refresh_height(pivot);
	return pivot;
}

static vma_node_t* rebalance(vma_node_t* node) {
	node_refresh_height(node);
	const int balance = node_balance_factor(node);

	if (balance > 1) {
		if (node_balance_factor(node->left) < 0)
			node->left = rotate_left(node->left);
		return rotate_right(node);
	}

	if (balance < -1) {
		if (node_balance_factor(node->right) > 0)
			node->right = rotate_right(node->right);
		return rotate_left(node);
	}

	return node;
}

static vma_node_t* node_insert(vma_node_t* root, vma_node_t* node, bool* inserted) {
	if (root == NULL) {
		*inserted = true;
		return node;
	}

	if (node->end <= root->start) {
		root->left = node_insert(root->left, node, inserted);
	} else if (node->start >= root->end) {
		root->right = node_insert(root->right, node, inserted);
	} else {
		*inserted = false;
		return root;
	}

	if (!*inserted)
		return root;

	return rebalance(root);
}

static vma_node_t* node_leftmost(vma_node_t* node) {
	while (node != NULL && node->left != NULL)
		node = node->left;
	return node;
}

static vma_node_t* node_remove(vma_node_t* root, uint32_t start, uint32_t end, bool* removed) {
	if (root == NULL)
		return NULL;

	if (start < root->start) {
		root->left = node_remove(root->left, start, end, removed);
	} else if (start > root->start) {
		root->right = node_remove(root->right, start, end, removed);
	} else {
		if (root->end != end) {
			*removed = false;
			return root;
		}

		*removed = true;
		if (root->left == NULL || root->right == NULL) {
			vma_node_t* replacement = root->left != NULL ? root->left : root->right;
			node_free(root);
			return replacement;
		}

		vma_node_t* successor = node_leftmost(root->right);
		root->start = successor->start;
		root->end = successor->end;
		root->flags = successor->flags;
		bool dropped = false;
		root->right = node_remove(root->right, successor->start, successor->end, &dropped);
	}

	if (root == NULL)
		return NULL;

	return rebalance(root);
}

static void node_release_subtree(vma_node_t* node) {
	if (node == NULL)
		return;

	node_release_subtree(node->left);
	node_release_subtree(node->right);
	node_free(node);
}

static void gap_search_in_order(vma_node_t* node, gap_search_state_t* state) {
	if (node == NULL || state->found)
		return;

	gap_search_in_order(node->left, state);
	if (state->found)
		return;

	if (node->end > state->cursor) {
		uint32_t gap_end = node->start;
		if (gap_end > state->max_addr)
			gap_end = state->max_addr;

		if (add_fits_limit(state->cursor, state->length, gap_end)) {
			state->result = state->cursor;
			state->found = true;
			return;
		}

		uint32_t next_cursor = align_up_u32(node->end, state->alignment);
		if (next_cursor < state->cursor)
			next_cursor = state->max_addr;
		state->cursor = next_cursor > state->max_addr ? state->max_addr : next_cursor;
	}

	gap_search_in_order(node->right, state);
}

void vma_tree_init(vma_tree_t* tree) {
	tree->root = NULL;
	tree->node_count = 0;
}

void vma_tree_clear(vma_tree_t* tree) {
	vma_node_t* root = (vma_node_t*) tree->root;
	node_release_subtree(root);
	tree->root = NULL;
	tree->node_count = 0;
}

bool vma_tree_insert(vma_tree_t* tree, uint32_t start, uint32_t end, uint32_t flags) {
	if (end <= start)
		return false;

	vma_node_t* node = node_alloc(start, end, flags);
	if (node == NULL)
		return false;

	bool inserted = false;
	tree->root = node_insert((vma_node_t*) tree->root, node, &inserted);
	if (!inserted) {
		node_free(node);
		return false;
	}

	tree->node_count++;
	return true;
}

bool vma_tree_remove(vma_tree_t* tree, uint32_t start, uint32_t end) {
	bool removed = false;
	tree->root = node_remove((vma_node_t*) tree->root, start, end, &removed);
	if (removed)
		tree->node_count--;
	return removed;
}

bool vma_tree_find(const vma_tree_t* tree, uint32_t address,
		uint32_t* start_out, uint32_t* end_out, uint32_t* flags_out) {
	vma_node_t* node = (vma_node_t*) tree->root;

	while (node != NULL) {
		if (address < node->start) {
			node = node->left;
		} else if (address >= node->end) {
			node = node->right;
		} else {
			if (start_out != NULL)
				*start_out = node->start;
			if (end_out != NULL)
				*end_out = node->end;
			if (flags_out != NULL)
				*flags_out = node->flags;
			return true;
		}
	}

	return false;
}

bool vma_tree_first(const vma_tree_t* tree,
		uint32_t* start_out, uint32_t* end_out, uint32_t* flags_out) {
	vma_node_t* node = node_leftmost((vma_node_t*) tree->root);
	if (node == NULL)
		return false;

	if (start_out != NULL)
		*start_out = node->start;
	if (end_out != NULL)
		*end_out = node->end;
	if (flags_out != NULL)
		*flags_out = node->flags;
	return true;
}

bool vma_tree_find_gap(const vma_tree_t* tree, uint32_t min_addr, uint32_t max_addr,
		uint32_t length, uint32_t alignment, uint32_t* start_out) {
	if (length == 0 || min_addr >= max_addr || alignment == 0)
		return false;

	if ((alignment & (alignment - 1u)) != 0)
		return false;

	const uint32_t start = align_up_u32(min_addr, alignment);
	if (start < min_addr || start >= max_addr)
		return false;

	gap_search_state_t state;
	state.cursor = start;
	state.max_addr = max_addr;
	state.length = length;
	state.alignment = alignment;
	state.result = 0;
	state.found = false;

	gap_search_in_order((vma_node_t*) tree->root, &state);
	if (!state.found && add_fits_limit(state.cursor, length, max_addr)) {
		state.result = state.cursor;
		state.found = true;
	}

	if (!state.found)
		return false;

	*start_out = state.result;
	return true;
}

uint32_t vma_tree_node_count(const vma_tree_t* tree) {
	return tree->node_count;
}
