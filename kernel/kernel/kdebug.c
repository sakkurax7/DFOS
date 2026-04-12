#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <kernel/boot.h>
#include <kernel/bootinfo.h>
#include <kernel/heap.h>
#include <kernel/input.h>
#include <kernel/kdebug.h>
#include <kernel/paging.h>
#include <kernel/pmm.h>
#include <kernel/scheduler.h>
#include <kernel/vma.h>
#include <kernel/vfs.h>

#define KDEBUG_LINE_MAX 128

static bool debugger_active;

static void kdebug_print_prompt(void) {
	printf("\n[kdebug] ");
}

static bool starts_with(const char* text, const char* prefix) {
	return strncmp(text, prefix, strlen(prefix)) == 0;
}

static void kdebug_show_tasks(void) {
	scheduler_task_snapshot_t snapshot;
	const uint32_t count = scheduler_task_count();

	for (uint32_t i = 0; i < count; i++) {
		if (!scheduler_get_task_snapshot(i, &snapshot))
			continue;
		printf("task %u %-12s state=%s wake=%u cr3=0x%x\n",
			snapshot.id, snapshot.name, snapshot.state, snapshot.wakeup_tick,
			snapshot.address_space_root);
	}
}

static void kdebug_list_files(void) {
	vfs_node_t node;
	for (uint32_t i = 0; i < vfs_file_count(); i++) {
		if (vfs_get_file(i, &node))
			printf("%s (%u bytes)\n", node.name, (unsigned int) node.size);
	}
}

static void kdebug_cat_file(const char* path) {
	vfs_node_t node;

	if (!vfs_open(path, &node)) {
		printf("no such file: %s\n", path);
		return;
	}

	printf("----- %s -----\n", node.name);
	for (size_t i = 0; i < node.size; i++)
		putchar(node.data[i]);
	if (node.size == 0 || node.data[node.size - 1] != '\n')
		putchar('\n');
}

static bool kdebug_test_memmap(void) {
	if (!bootinfo_has_memory_map()) {
		printf("memmap: bootloader did not provide a memory map\n");
		return false;
	}

	bootinfo_memory_iterator_t iterator;
	bootinfo_memory_region_t region;
	uint32_t entries = 0;
	uint64_t available_bytes = 0;

	for (bool has_entry = bootinfo_memory_begin(&iterator, &region);
			has_entry; has_entry = bootinfo_memory_next(&iterator, &region)) {
		entries++;
		if (region.type == MULTIBOOT_MEMORY_AVAILABLE)
			available_bytes += region.length;
	}

	const bool pass = entries > 0 && available_bytes > 0;
	printf("memmap: entries=%u available=%u MiB -> %s\n",
		entries, (uint32_t) (available_bytes >> 20), pass ? "PASS" : "FAIL");
	return pass;
}

static bool kdebug_test_vma_tree(void) {
	vma_tree_t tree;
	vma_tree_init(&tree);

	bool pass = true;
	pass = pass && vma_tree_insert(&tree, 0x1000u, 0x3000u, 1u);
	pass = pass && vma_tree_insert(&tree, 0x5000u, 0x7000u, 2u);
	pass = pass && !vma_tree_insert(&tree, 0x2800u, 0x3800u, 3u);

	uint32_t gap = 0;
	pass = pass && vma_tree_find_gap(&tree, 0x1000u, 0x9000u, 0x1000u, 0x1000u, &gap);
	pass = pass && gap == 0x3000u;

	uint32_t start = 0;
	uint32_t end = 0;
	uint32_t flags = 0;
	pass = pass && vma_tree_find(&tree, 0x5200u, &start, &end, &flags);
	pass = pass && start == 0x5000u && end == 0x7000u && flags == 2u;
	pass = pass && vma_tree_remove(&tree, 0x5000u, 0x7000u);

	vma_tree_clear(&tree);
	printf("vma: AVL insert/find/remove/gap checks -> %s\n", pass ? "PASS" : "FAIL");
	return pass;
}

static bool kdebug_test_slab(void) {
	void* a = kmalloc(24);
	void* b = kmalloc(96);
	void* c = kmalloc(1600);
	void* d = kmalloc(5000);

	memset(a, 0xA5, 24);
	memset(b, 0x5A, 96);
	memset(c, 0x3C, 1600);
	memset(d, 0xC3, 5000);

	kfree(d);
	kfree(c);
	kfree(b);
	kfree(a);

	printf("slab: mixed-size alloc/free cycle -> PASS\n");
	return true;
}

static bool kdebug_test_address_spaces(void) {
	const uint32_t test_virtual = PAGING_USER_BASE + PAGE_SIZE;
	paging_space_t* space_a = paging_create_process_space();
	paging_space_t* space_b = paging_create_process_space();
	uint32_t frame_a = 0;
	uint32_t frame_b = 0;
	bool frame_a_valid = false;
	bool frame_b_valid = false;
	bool pass = true;

	if (space_a == NULL || space_b == NULL) {
		printf("aspace: failed to allocate process spaces\n");
		pass = false;
		goto cleanup;
	}

	if (!pmm_alloc_frame(&frame_a) || !pmm_alloc_frame(&frame_b)) {
		printf("aspace: failed to allocate physical frames\n");
		pass = false;
		goto cleanup;
	}

	frame_a_valid = true;
	frame_b_valid = true;

	pass = pass && paging_map_user_page(space_a, (void*) (uintptr_t) test_virtual,
		frame_a, PAGING_FLAG_WRITABLE);
	pass = pass && paging_map_user_page(space_b, (void*) (uintptr_t) test_virtual,
		frame_b, PAGING_FLAG_WRITABLE);

	uint32_t resolved_a = 0;
	uint32_t resolved_b = 0;
	pass = pass && paging_lookup_physical(space_a, (const void*) (uintptr_t) test_virtual, &resolved_a);
	pass = pass && paging_lookup_physical(space_b, (const void*) (uintptr_t) test_virtual, &resolved_b);
	pass = pass && resolved_a == frame_a;
	pass = pass && resolved_b == frame_b;
	pass = pass && resolved_a != resolved_b;

cleanup:
	if (space_a != NULL)
		paging_unmap_user_page(space_a, (void*) (uintptr_t) test_virtual);
	if (space_b != NULL)
		paging_unmap_user_page(space_b, (void*) (uintptr_t) test_virtual);
	if (frame_a_valid)
		pmm_free_frame(frame_a);
	if (frame_b_valid)
		pmm_free_frame(frame_b);
	if (space_a != NULL)
		paging_destroy_process_space(space_a);
	if (space_b != NULL)
		paging_destroy_process_space(space_b);

	printf("aspace: per-process mapping isolation -> %s\n", pass ? "PASS" : "FAIL");
	return pass;
}

static void kdebug_run_tests(const char* selector) {
	bool run_all = selector == NULL || selector[0] == '\0' || strcmp(selector, "all") == 0;
	bool pass = true;
	uint32_t executed = 0;

	if (run_all || strcmp(selector, "memmap") == 0) {
		pass = kdebug_test_memmap() && pass;
		executed++;
	}

	if (run_all || strcmp(selector, "vma") == 0) {
		pass = kdebug_test_vma_tree() && pass;
		executed++;
	}

	if (run_all || strcmp(selector, "slab") == 0) {
		pass = kdebug_test_slab() && pass;
		executed++;
	}

	if (run_all || strcmp(selector, "aspace") == 0) {
		pass = kdebug_test_address_spaces() && pass;
		executed++;
	}

	if (executed == 0) {
		printf("tests: unknown selector '%s' (use memmap, vma, slab, aspace, all)\n", selector);
		return;
	}

	printf("tests: %u executed -> %s\n", executed, pass ? "PASS" : "FAIL");
}

static void kdebug_execute(char* line) {
	if (strcmp(line, "help") == 0) {
		printf("commands: help, tasks, mem, ls, cat <file>, test [all|memmap|vma|slab|aspace], continue\n");
	} else if (strcmp(line, "tasks") == 0) {
		kdebug_show_tasks();
	} else if (strcmp(line, "mem") == 0) {
		printf("memory: total=%u KiB free=%u KiB\n",
			pmm_total_memory_kib(), pmm_free_memory_kib());
	} else if (strcmp(line, "ls") == 0) {
		kdebug_list_files();
	} else if (starts_with(line, "cat ")) {
		kdebug_cat_file(line + 4);
	} else if (strcmp(line, "test") == 0) {
		kdebug_run_tests("all");
	} else if (starts_with(line, "test ")) {
		kdebug_run_tests(line + 5);
	} else if (strcmp(line, "continue") == 0) {
		debugger_active = false;
		printf("leaving debugger\n");
		return;
	} else if (line[0] != '\0') {
		printf("unknown command: %s\n", line);
	}

	if (debugger_active)
		kdebug_print_prompt();
}

static void kdebug_task(void* arg) {
	(void) arg;
	char line[KDEBUG_LINE_MAX];
	size_t line_length = 0;

	while (true) {
		if (!debugger_active && input_debug_requested()) {
			input_clear_debug_request();
			debugger_active = true;
			printf("\nentered kernel debugger, type 'help'\n");
			kdebug_print_prompt();
		}

		if (!debugger_active) {
			scheduler_sleep(1);
			continue;
		}

		char c;
		if (!input_read_char_nonblocking(&c)) {
			// The debugger is a normal scheduler task, so it sleeps instead of spinning.
			scheduler_sleep(1);
			continue;
		}

		if (c == '\r')
			c = '\n';

		if (c == '\n') {
			putchar('\n');
			line[line_length] = '\0';
			// Commands are interpreted in-place from the line buffer to keep the debugger tiny.
			kdebug_execute(line);
			line_length = 0;
			continue;
		}

		if (c == '\b') {
			if (line_length > 0) {
				line_length--;
				putchar('\b');
			}
			continue;
		}

		if (line_length + 1 < sizeof(line)) {
			line[line_length++] = c;
			putchar(c);
		}
	}
}

void kdebug_init(void) {
	debugger_active = false;
	scheduler_create_kernel_task("kdebug", kdebug_task, NULL);
}
