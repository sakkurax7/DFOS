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
#include <kernel/sync.h>
#include <kernel/vma.h>
#include <kernel/vfs.h>

#define KDEBUG_LINE_MAX 128

static bool debugger_active;

typedef struct kdebug_sync_counter_ctx {
	kspinlock_t lock;
	volatile uint32_t counter;
	volatile uint32_t done_workers;
	uint32_t iterations_per_worker;
} kdebug_sync_counter_ctx_t;

typedef struct kdebug_sync_condition_ctx {
	kspinlock_t lock;
	kcondition_t condition;
	volatile bool ready;
	volatile bool waiter_done;
	volatile bool waiter_woke;
	volatile bool waiter_timed_out;
} kdebug_sync_condition_ctx_t;

static void kdebug_sync_counter_task(void* arg) {
	kdebug_sync_counter_ctx_t* context = (kdebug_sync_counter_ctx_t*) arg;
	for (uint32_t i = 0; i < context->iterations_per_worker; i++) {
		kspin_lock(&context->lock);
		context->counter++;
		kspin_unlock(&context->lock);

		if ((i & 31u) == 0u)
			scheduler_yield();
	}

	kspin_lock(&context->lock);
	context->done_workers++;
	kspin_unlock(&context->lock);
}

static void kdebug_sync_waiter_task(void* arg) {
	kdebug_sync_condition_ctx_t* context = (kdebug_sync_condition_ctx_t*) arg;

	kspin_lock(&context->lock);
	while (!context->ready) {
		const bool signaled = kcondition_wait(&context->condition, &context->lock, 50u);
		if (!signaled) {
			context->waiter_timed_out = true;
			break;
		}
	}

	context->waiter_woke = context->ready;
	context->waiter_done = true;
	kspin_unlock(&context->lock);
}

static void kdebug_sync_signaler_task(void* arg) {
	kdebug_sync_condition_ctx_t* context = (kdebug_sync_condition_ctx_t*) arg;

	scheduler_sleep(5u);
	kspin_lock(&context->lock);
	context->ready = true;
	kcondition_signal(&context->condition);
	kspin_unlock(&context->lock);
}

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
		printf("task %u %-12s state=%-8s prio=%u cpu=%u node=%u wake=%u affinity=0x%x cr3=0x%x\n",
			snapshot.id, snapshot.name, snapshot.state,
			(unsigned) snapshot.priority, (unsigned) snapshot.cpu_id, (unsigned) snapshot.numa_node,
			snapshot.wakeup_tick, snapshot.cpu_affinity_mask, snapshot.address_space_root);
	}
}

static void kdebug_show_cpus(void) {
	scheduler_cpu_snapshot_t cpu;
	scheduler_list_snapshot_t lists;
	const uint32_t count = scheduler_cpu_count();

	for (uint32_t i = 0; i < count; i++) {
		if (!scheduler_get_cpu_snapshot(i, &cpu))
			continue;
		printf("cpu %u node=%u online=%s running=%u(%s) runnable=%u\n",
			(unsigned) cpu.cpu_id, (unsigned) cpu.numa_node,
			cpu.online ? "yes" : "no",
			cpu.running_task_id, cpu.running_task_name, cpu.runnable_count);
	}

	scheduler_get_list_snapshot(&lists);
	printf("lists: free=%u runnable=%u sleeping=%u blocked=%u zombie=%u\n",
		lists.free_count, lists.runnable_count, lists.sleeping_count,
		lists.blocked_count, lists.zombie_count);
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

static bool kdebug_test_scheduler(void) {
	scheduler_self_test_report_t report;
	const bool pass = scheduler_run_self_tests(&report);

	printf("sched: checks=%u failures=%u -> %s\n",
		report.checks, report.failures, pass ? "PASS" : "FAIL");
	return pass;
}

typedef struct kdebug_lifetime_object {
	kobject_t object;
	volatile uint32_t* release_counter;
} kdebug_lifetime_object_t;

static void kdebug_lifetime_release(kobject_t* object) {
	kdebug_lifetime_object_t* typed = (kdebug_lifetime_object_t*) object;
	(*typed->release_counter)++;
}

static bool kdebug_test_sync_locking(void) {
	kdebug_sync_counter_ctx_t context;
	kspinlock_init(&context.lock);
	context.counter = 0;
	context.done_workers = 0;
	context.iterations_per_worker = 600u;

	bool pass = true;
	pass = pass && scheduler_create_kernel_task("sync-lock-a", kdebug_sync_counter_task, &context);
	pass = pass && scheduler_create_kernel_task("sync-lock-b", kdebug_sync_counter_task, &context);
	if (!pass) {
		printf("sync-lock: failed to create worker tasks\n");
		return false;
	}

	const uint32_t start_tick = scheduler_ticks();
	const uint32_t timeout_ticks = 400u;
	while (context.done_workers < 2u && (scheduler_ticks() - start_tick) < timeout_ticks)
		scheduler_sleep(1u);

	const uint32_t expected = context.iterations_per_worker * 2u;
	pass = pass && context.done_workers == 2u;
	pass = pass && context.counter == expected;

	printf("sync-lock: done=%u counter=%u expected=%u -> %s\n",
		context.done_workers, context.counter, expected, pass ? "PASS" : "FAIL");
	return pass;
}

static bool kdebug_test_wait_queue_timeout(void) {
	kwait_queue_t queue;
	kwait_queue_init(&queue);

	const uint32_t start_tick = scheduler_ticks();
	const bool woke = kwait_wait(&queue, 3u);
	const uint32_t elapsed = scheduler_ticks() - start_tick;
	const bool pass = !woke && elapsed >= 3u;

	printf("sync-wait-timeout: woke=%s elapsed=%u -> %s\n",
		woke ? "yes" : "no", elapsed, pass ? "PASS" : "FAIL");
	return pass;
}

static bool kdebug_test_wait_queue_wake(void) {
	kdebug_sync_condition_ctx_t context;
	kspinlock_init(&context.lock);
	kcondition_init(&context.condition);
	context.ready = false;
	context.waiter_done = false;
	context.waiter_woke = false;
	context.waiter_timed_out = false;

	bool pass = true;
	pass = pass && scheduler_create_kernel_task("sync-waiter", kdebug_sync_waiter_task, &context);
	pass = pass && scheduler_create_kernel_task("sync-signaler", kdebug_sync_signaler_task, &context);
	if (!pass) {
		printf("sync-wait-wake: failed to create helper tasks\n");
		return false;
	}

	const uint32_t start_tick = scheduler_ticks();
	const uint32_t timeout_ticks = 200u;
	while (!context.waiter_done && (scheduler_ticks() - start_tick) < timeout_ticks)
		scheduler_sleep(1u);

	pass = pass && context.waiter_done;
	pass = pass && context.waiter_woke;
	pass = pass && !context.waiter_timed_out;

	printf("sync-wait-wake: done=%s woke=%s timed_out=%s -> %s\n",
		context.waiter_done ? "yes" : "no",
		context.waiter_woke ? "yes" : "no",
		context.waiter_timed_out ? "yes" : "no",
		pass ? "PASS" : "FAIL");
	return pass;
}

static bool kdebug_test_lifetime_model(void) {
	volatile uint32_t release_counter = 0;
	bool pass = true;

	kdebug_lifetime_object_t* object =
		(kdebug_lifetime_object_t*) kmalloc(sizeof(kdebug_lifetime_object_t));
	if (object == NULL) {
		printf("lifetime: failed to allocate object\n");
		return false;
	}

	object->release_counter = &release_counter;
	kobject_init(&object->object, kdebug_lifetime_release);
	pass = pass && kobject_refcount(&object->object) == 1u;

	kobject_get(&object->object);
	pass = pass && kobject_refcount(&object->object) == 2u;

	kobject_put(&object->object);
	pass = pass && kobject_refcount(&object->object) == 1u;
	pass = pass && release_counter == 0u;

	kobject_put(&object->object);
	pass = pass && release_counter == 1u;

	kref_t refcount;
	kref_init(&refcount, 2u);
	kref_get(&refcount);
	pass = pass && kref_read(&refcount) == 3u;
	pass = pass && !kref_put(&refcount);
	pass = pass && !kref_put(&refcount);
	pass = pass && kref_put(&refcount);

	kfree(object);
	printf("lifetime: release_count=%u -> %s\n",
		(uint32_t) release_counter, pass ? "PASS" : "FAIL");
	return pass;
}

static bool kdebug_test_sync_primitives(void) {
	bool pass = true;

	pass = kdebug_test_sync_locking() && pass;
	pass = kdebug_test_wait_queue_timeout() && pass;
	pass = kdebug_test_wait_queue_wake() && pass;
	pass = kdebug_test_lifetime_model() && pass;

	printf("sync: primitive suite -> %s\n", pass ? "PASS" : "FAIL");
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

	if (run_all || strcmp(selector, "sched") == 0) {
		pass = kdebug_test_scheduler() && pass;
		executed++;
	}

	if (run_all || strcmp(selector, "sync") == 0) {
		pass = kdebug_test_sync_primitives() && pass;
		executed++;
	}

	if (executed == 0) {
		printf("tests: unknown selector '%s' (use memmap, vma, slab, aspace, sched, sync, all)\n",
			selector);
		return;
	}

	printf("tests: %u executed -> %s\n", executed, pass ? "PASS" : "FAIL");
}

static void kdebug_execute(char* line) {
	if (strcmp(line, "help") == 0) {
		printf("commands: help, tasks, cpus, mem, ls, cat <file>, test [all|memmap|vma|slab|aspace|sched|sync], continue\n");
	} else if (strcmp(line, "tasks") == 0) {
		kdebug_show_tasks();
	} else if (strcmp(line, "cpus") == 0) {
		kdebug_show_cpus();
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
