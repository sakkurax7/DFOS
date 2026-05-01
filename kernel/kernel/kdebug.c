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
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/scheduler.h>
#include <kernel/sync.h>
#include <kernel/test.h>
#include <kernel/vfs.h>
#include <kernel/vma.h>
#include <kernel/x86.h>

#define KDEBUG_LINE_MAX 128
#define X86_EFLAGS_INTERRUPT_FLAG (1u << 9)

static bool debugger_active;
static bool tests_registered;

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

typedef struct kdebug_mutex_counter_ctx {
	kmutex_t mutex;
	volatile uint32_t counter;
	volatile uint32_t done_workers;
	uint32_t iterations_per_worker;
} kdebug_mutex_counter_ctx_t;

typedef struct kdebug_mutex_timeout_ctx {
	kmutex_t mutex;
	volatile bool worker_done;
	volatile bool worker_acquired;
	volatile bool worker_timed_out;
} kdebug_mutex_timeout_ctx_t;

typedef struct kdebug_scheduler_join_ctx {
	volatile uint32_t task_id;
	volatile bool exited;
} kdebug_scheduler_join_ctx_t;

typedef struct kdebug_lifetime_object {
	kobject_t object;
	volatile uint32_t* release_counter;
} kdebug_lifetime_object_t;

static bool starts_with(const char* text, const char* prefix) {
	return strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool kdebug_test_scheduler_join(void);

static uint32_t kdebug_zombie_count(void) {
	scheduler_list_snapshot_t lists;
	scheduler_get_list_snapshot(&lists);
	return lists.zombie_count;
}

static bool kdebug_reap_zombies_to_target(uint32_t target_zombies, uint32_t timeout_ticks) {
	const uint32_t start_tick = scheduler_ticks();
	while (kdebug_zombie_count() > target_zombies) {
		(void) scheduler_reap_zombies(0);
		if (kdebug_zombie_count() <= target_zombies)
			return true;

		if ((scheduler_ticks() - start_tick) >= timeout_ticks)
			break;
		scheduler_sleep(1u);
	}

	(void) scheduler_reap_zombies(0);
	return kdebug_zombie_count() <= target_zombies;
}

static void kdebug_stats_add(ktest_stats_t* stats, uint32_t checks, uint32_t failures) {
	if (stats == NULL)
		return;

	stats->checks += checks;
	stats->failures += failures;
}

static bool kdebug_record_subtest(ktest_stats_t* stats, bool pass) {
	ktest_stats_record(stats, pass);
	return pass;
}

static void kdebug_print_prompt(void) {
	printf("\n[kdebug] ");
}

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

static void kdebug_mutex_counter_task(void* arg) {
	kdebug_mutex_counter_ctx_t* context = (kdebug_mutex_counter_ctx_t*) arg;

	for (uint32_t i = 0; i < context->iterations_per_worker; i++) {
		kmutex_lock(&context->mutex);
		context->counter++;
		kmutex_unlock(&context->mutex);

		if ((i & 31u) == 0u)
			scheduler_yield();
	}

	kmutex_lock(&context->mutex);
	context->done_workers++;
	kmutex_unlock(&context->mutex);
}

static void kdebug_mutex_timeout_task(void* arg) {
	kdebug_mutex_timeout_ctx_t* context = (kdebug_mutex_timeout_ctx_t*) arg;

	const bool acquired = kmutex_lock_timeout(&context->mutex, 5u);
	context->worker_acquired = acquired;
	context->worker_timed_out = !acquired;
	if (acquired)
		kmutex_unlock(&context->mutex);
	context->worker_done = true;
}

static void kdebug_scheduler_join_task(void* arg) {
	kdebug_scheduler_join_ctx_t* context = (kdebug_scheduler_join_ctx_t*) arg;
	if (context == NULL)
		return;

	context->task_id = scheduler_current_task_id();
	scheduler_sleep(2u);
	context->exited = true;
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

static void kdebug_show_numa(void) {
	const uint8_t node_count = pmm_numa_node_count();
	printf("numa: nodes=%u\n", (unsigned) node_count);

	for (uint8_t node = 0; node < node_count; node++) {
		pmm_numa_node_stats_t stats;
		if (!pmm_get_numa_node_stats(node, &stats))
			continue;

		const uint64_t start_phys = (uint64_t) stats.first_frame * PAGE_SIZE;
		const uint64_t end_phys = start_phys + (uint64_t) stats.frame_count * PAGE_SIZE;
		printf("node %u: frames=%u free=%u range=[0x%llx, 0x%llx)\n",
			(unsigned) stats.node_id, stats.frame_count, stats.free_frames,
			(unsigned long long) start_phys, (unsigned long long) end_phys);
	}
}

static void kdebug_print_latency_metric(
	const char* name, const scheduler_trace_latency_metric_t* metric) {
	if (name == NULL || metric == NULL)
		return;

	printf("%s: samples=%llu total=%llu min=%u max=%u\n",
		name,
		(unsigned long long) metric->sample_count,
		(unsigned long long) metric->total_ticks,
		metric->min_ticks,
		metric->max_ticks);
}

static void kdebug_show_schedstat(void) {
	scheduler_trace_counters_t counters = { 0 };
	scheduler_trace_latency_t latency = { 0 };
	if (!scheduler_get_trace_counters(&counters) || !scheduler_get_trace_latency(&latency)) {
		printf("schedstat: unavailable\n");
		return;
	}

	printf("schedstat: ticks=%llu schedule=%llu switches=%llu preempt=%llu yields=%llu wakeups=%llu\n",
		(unsigned long long) counters.timer_ticks,
		(unsigned long long) counters.schedule_events,
		(unsigned long long) counters.context_switches,
		(unsigned long long) counters.preemptions,
		(unsigned long long) counters.voluntary_yields,
		(unsigned long long) counters.wakeups);
	printf("schedstat: waits=%llu timeouts=%llu sleeps=%llu creates=%llu exits=%llu prio_updates=%llu\n",
		(unsigned long long) counters.wait_calls,
		(unsigned long long) counters.wait_timeouts,
		(unsigned long long) counters.sleep_calls,
		(unsigned long long) counters.task_creations,
		(unsigned long long) counters.task_exits,
		(unsigned long long) counters.priority_updates);
	printf("schedstat: balance_runs=%llu balance_moves=%llu migrations=%llu remote_wake_ipi=%llu remote_resched_ipi=%llu\n",
		(unsigned long long) counters.load_balance_runs,
		(unsigned long long) counters.load_balance_migrations,
		(unsigned long long) counters.task_migrations,
		(unsigned long long) counters.remote_wakeup_ipis,
		(unsigned long long) counters.remote_reschedule_ipis);

	kdebug_print_latency_metric("runnable_wait_ticks", &latency.runnable_wait_ticks);
	kdebug_print_latency_metric("sleep_overshoot_ticks", &latency.sleep_overshoot_ticks);
	kdebug_print_latency_metric(
		"wait_timeout_overshoot_ticks", &latency.wait_timeout_overshoot_ticks);
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

static bool kdebug_test_memmap(ktest_stats_t* stats) {
	if (!bootinfo_has_memory_map()) {
		printf("memmap: bootloader did not provide a memory map\n");
		ktest_stats_record(stats, false);
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
	printf("memmap: entries=%u available=%u MiB\n",
		entries, (uint32_t) (available_bytes >> 20));
	ktest_stats_record(stats, pass);
	return pass;
}

static bool kdebug_test_vma_tree(ktest_stats_t* stats) {
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
	printf("vma: gap=%u start=0x%x end=0x%x flags=0x%x\n", gap, start, end, flags);
	ktest_stats_record(stats, pass);
	return pass;
}

static bool kdebug_test_slab(ktest_stats_t* stats) {
	void* a = kmalloc(24);
	void* b = kmalloc(96);
	void* c = kmalloc(1600);
	void* d = kmalloc(5000);

	const bool allocated = a != NULL && b != NULL && c != NULL && d != NULL;
	if (!allocated) {
		printf("slab: allocation failed a=%p b=%p c=%p d=%p\n", a, b, c, d);
		kfree(d);
		kfree(c);
		kfree(b);
		kfree(a);
		ktest_stats_record(stats, false);
		return false;
	}

	memset(a, 0xA5, 24);
	memset(b, 0x5A, 96);
	memset(c, 0x3C, 1600);
	memset(d, 0xC3, 5000);

	kfree(d);
	kfree(c);
	kfree(b);
	kfree(a);

	printf("slab: mixed-size alloc/free cycle complete\n");
	ktest_stats_record(stats, true);
	return true;
}

static bool kdebug_test_address_spaces(ktest_stats_t* stats) {
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

	printf("aspace: resolved_a=0x%x resolved_b=0x%x\n", resolved_a, resolved_b);

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

	ktest_stats_record(stats, pass);
	return pass;
}

static bool kdebug_test_scheduler(ktest_stats_t* stats) {
	scheduler_self_test_report_t report;
	bool pass = scheduler_run_self_tests(&report);

	kdebug_stats_add(stats, report.checks, report.failures);
	scheduler_trace_counters_t counters;
	scheduler_trace_latency_t latency;
	const bool trace_ok =
		scheduler_get_trace_counters(&counters) && scheduler_get_trace_latency(&latency);
	ktest_stats_record(stats, trace_ok);
	pass = pass && trace_ok;
	const bool join_ok = kdebug_test_scheduler_join();
	ktest_stats_record(stats, join_ok);
	pass = pass && join_ok;

	printf("sched: checks=%u failures=%u trace=%s join=%s schedule=%llu switches=%llu\n",
		report.checks, report.failures, trace_ok ? "ok" : "fail",
		join_ok ? "ok" : "fail",
		(unsigned long long) counters.schedule_events,
		(unsigned long long) counters.context_switches);
	return pass;
}

static bool kdebug_test_scheduler_join(void) {
	kdebug_scheduler_join_ctx_t context;
	context.task_id = 0;
	context.exited = false;

	if (!scheduler_create_kernel_task("sched-join", kdebug_scheduler_join_task, &context)) {
		printf("sched-join: failed to create join target task\n");
		return false;
	}

	const uint32_t wait_id_start = scheduler_ticks();
	while (context.task_id == 0 && (scheduler_ticks() - wait_id_start) < 100u)
		scheduler_sleep(1u);

	if (context.task_id == 0) {
		printf("sched-join: task id was not published by target\n");
		(void) scheduler_reap_zombies(0);
		return false;
	}

	const bool joined = scheduler_join_task(context.task_id, 200u);
	const bool pass = joined && context.exited;
	if (!joined)
		(void) scheduler_reap_zombies(0);

	printf("sched-join: id=%u joined=%s exited=%s\n",
		context.task_id, joined ? "yes" : "no", context.exited ? "yes" : "no");
	return pass;
}

static bool kdebug_test_pmm_numa(ktest_stats_t* stats) {
	const uint8_t node_count = pmm_numa_node_count();
	bool pass = node_count > 0;
	uint8_t preferred_node = PMM_NUMA_NODE_ANY;
	pmm_numa_node_stats_t preferred_stats = { 0 };

	for (uint8_t node = 0; node < node_count; node++) {
		pmm_numa_node_stats_t snapshot;
		if (!pmm_get_numa_node_stats(node, &snapshot))
			continue;

		if (snapshot.frame_count == 0)
			continue;

		preferred_node = node;
		preferred_stats = snapshot;
		break;
	}

	if (preferred_node == PMM_NUMA_NODE_ANY) {
		printf("pmm: no NUMA node has physical frames\n");
		ktest_stats_record(stats, false);
		return false;
	}

	uint32_t preferred_frame = 0;
	if (!pmm_alloc_frame_on_node(preferred_node, &preferred_frame)) {
		printf("pmm: preferred allocation failed on node %u\n", (unsigned) preferred_node);
		ktest_stats_record(stats, false);
		return false;
	}

	const uint8_t actual_node = pmm_numa_node_for_physical(preferred_frame);
	if (preferred_stats.free_frames > 0)
		pass = pass && actual_node == preferred_node;
	else
		pass = pass && actual_node != PMM_NUMA_NODE_ANY;

	pmm_free_frame(preferred_frame);

	uint32_t fallback_frame = 0;
	const bool fallback_ok = pmm_alloc_frame_on_node(node_count, &fallback_frame);
	pass = pass && fallback_ok;
	if (fallback_ok)
		pmm_free_frame(fallback_frame);

	printf("pmm: nodes=%u preferred=%u actual=%u fallback=%s\n",
		(unsigned) node_count,
		(unsigned) preferred_node,
		(unsigned) actual_node,
		fallback_ok ? "ok" : "fail");
	ktest_stats_record(stats, pass);
	return pass;
}

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
	const uint32_t baseline_zombies = kdebug_zombie_count();

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
	const bool reaped = kdebug_reap_zombies_to_target(baseline_zombies, 100u);
	pass = pass && reaped;

	printf("sync-lock: done=%u counter=%u expected=%u reaped=%s\n",
		context.done_workers, context.counter, expected, reaped ? "yes" : "no");
	return pass;
}

static bool kdebug_test_irqsave_spinlock(void) {
	kspinlock_t lock;
	kspinlock_init(&lock);

	const uint32_t before_flags = x86_read_eflags();
	const kirq_state_t irq_state = kspin_lock_irqsave(&lock);
	const uint32_t inside_flags = x86_read_eflags();
	kspin_unlock_irqrestore(&lock, irq_state);
	const uint32_t after_flags = x86_read_eflags();

	const bool before_enabled = (before_flags & X86_EFLAGS_INTERRUPT_FLAG) != 0;
	const bool inside_enabled = (inside_flags & X86_EFLAGS_INTERRUPT_FLAG) != 0;
	const bool after_enabled = (after_flags & X86_EFLAGS_INTERRUPT_FLAG) != 0;

	const bool pass = !inside_enabled && (after_enabled == before_enabled);
	printf("sync-irq: before=%s inside=%s after=%s\n",
		before_enabled ? "on" : "off",
		inside_enabled ? "on" : "off",
		after_enabled ? "on" : "off");
	return pass;
}

static bool kdebug_test_wait_queue_timeout(void) {
	kwait_queue_t queue;
	kwait_queue_init(&queue);

	const uint32_t start_tick = scheduler_ticks();
	const bool woke = kwait_wait(&queue, 3u);
	const uint32_t elapsed = scheduler_ticks() - start_tick;
	const bool pass = !woke && elapsed >= 3u;

	printf("sync-wait-timeout: woke=%s elapsed=%u\n", woke ? "yes" : "no", elapsed);
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
	const uint32_t baseline_zombies = kdebug_zombie_count();

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
	const bool reaped = kdebug_reap_zombies_to_target(baseline_zombies, 100u);
	pass = pass && reaped;

	printf("sync-wait-wake: done=%s woke=%s timed_out=%s reaped=%s\n",
		context.waiter_done ? "yes" : "no",
		context.waiter_woke ? "yes" : "no",
		context.waiter_timed_out ? "yes" : "no",
		reaped ? "yes" : "no");
	return pass;
}

static bool kdebug_test_mutex_locking(void) {
	kdebug_mutex_counter_ctx_t context;
	kmutex_init(&context.mutex);
	context.counter = 0;
	context.done_workers = 0;
	context.iterations_per_worker = 400u;
	const uint32_t baseline_zombies = kdebug_zombie_count();

	bool pass = true;
	pass = pass && scheduler_create_kernel_task("sync-mutex-a", kdebug_mutex_counter_task, &context);
	pass = pass && scheduler_create_kernel_task("sync-mutex-b", kdebug_mutex_counter_task, &context);
	if (!pass) {
		printf("sync-mutex: failed to create worker tasks\n");
		return false;
	}

	const uint32_t start_tick = scheduler_ticks();
	const uint32_t timeout_ticks = 400u;
	while (context.done_workers < 2u && (scheduler_ticks() - start_tick) < timeout_ticks)
		scheduler_sleep(1u);

	const uint32_t expected = context.iterations_per_worker * 2u;
	pass = pass && context.done_workers == 2u;
	pass = pass && context.counter == expected;
	const bool reaped = kdebug_reap_zombies_to_target(baseline_zombies, 100u);
	pass = pass && reaped;

	printf("sync-mutex: done=%u counter=%u expected=%u reaped=%s\n",
		context.done_workers, context.counter, expected, reaped ? "yes" : "no");
	return pass;
}

static bool kdebug_test_mutex_timeout(void) {
	kdebug_mutex_timeout_ctx_t context;
	kmutex_init(&context.mutex);
	context.worker_done = false;
	context.worker_acquired = false;
	context.worker_timed_out = false;
	const uint32_t baseline_zombies = kdebug_zombie_count();

	kmutex_lock(&context.mutex);
	bool pass = scheduler_create_kernel_task("sync-mutex-timeout", kdebug_mutex_timeout_task, &context);
	if (!pass) {
		kmutex_unlock(&context.mutex);
		printf("sync-mutex-timeout: failed to create worker task\n");
		return false;
	}

	const uint32_t start_tick = scheduler_ticks();
	const uint32_t timeout_ticks = 100u;
	while (!context.worker_done && (scheduler_ticks() - start_tick) < timeout_ticks)
		scheduler_sleep(1u);
	kmutex_unlock(&context.mutex);

	pass = pass && context.worker_done;
	pass = pass && !context.worker_acquired;
	pass = pass && context.worker_timed_out;
	const bool reaped = kdebug_reap_zombies_to_target(baseline_zombies, 100u);
	pass = pass && reaped;

	printf("sync-mutex-timeout: done=%s acquired=%s timed_out=%s reaped=%s\n",
		context.worker_done ? "yes" : "no",
		context.worker_acquired ? "yes" : "no",
		context.worker_timed_out ? "yes" : "no",
		reaped ? "yes" : "no");
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
	printf("lifetime: release_count=%u\n", (uint32_t) release_counter);
	return pass;
}

static bool kdebug_test_sync_primitives(ktest_stats_t* stats) {
	bool pass = true;

	pass = kdebug_record_subtest(stats, kdebug_test_sync_locking()) && pass;
	pass = kdebug_record_subtest(stats, kdebug_test_irqsave_spinlock()) && pass;
	pass = kdebug_record_subtest(stats, kdebug_test_wait_queue_timeout()) && pass;
	pass = kdebug_record_subtest(stats, kdebug_test_wait_queue_wake()) && pass;
	pass = kdebug_record_subtest(stats, kdebug_test_mutex_locking()) && pass;
	pass = kdebug_record_subtest(stats, kdebug_test_mutex_timeout()) && pass;
	pass = kdebug_record_subtest(stats, kdebug_test_lifetime_model()) && pass;
	return pass;
}

static bool kdebug_test_panic_log(ktest_stats_t* stats) {
	panic_log_capture("panic-log-self-test run=%u", 1u);
	panic_log_capture("panic-log-self-test run=%u", 2u);

	panic_record_t newest = { 0 };
	panic_record_t older = { 0 };
	bool pass = true;
	pass = pass && panic_log_count() >= 2u;
	pass = pass && panic_log_read(0u, &newest);
	pass = pass && panic_log_read(1u, &older);
	if (pass) {
		pass = pass && newest.sequence > older.sequence;
		pass = pass && strcmp(newest.message, "panic-log-self-test run=2") == 0;
		pass = pass && strcmp(older.message, "panic-log-self-test run=1") == 0;
	}

	printf("panic: records=%u newest_seq=%u\n", panic_log_count(), newest.sequence);
	ktest_stats_record(stats, pass);
	return pass;
}

static void kdebug_show_panic_log(void) {
	const uint32_t count = panic_log_count();
	if (count == 0) {
		printf("paniclog: empty\n");
		return;
	}

	for (uint32_t i = 0; i < count; i++) {
		panic_record_t record;
		if (!panic_log_read(i, &record))
			continue;

		printf("paniclog[%u]: seq=%u tick=%u task=%u(%s)%s\n",
			i, record.sequence, record.tick, record.task_id, record.task_name,
			record.truncated ? " [truncated]" : "");
		printf("  %s\n", record.message);
	}
}

static void kdebug_test_reporter(const char* name, const ktest_stats_t* stats,
	bool pass, void* context) {
	(void) context;
	printf("test:%-8s checks=%u failures=%u -> %s\n",
		name, stats->checks, stats->failures, pass ? "PASS" : "FAIL");
}

static void kdebug_print_test_selectors(void) {
	const uint32_t count = ktest_registered_count();
	if (count == 0) {
		printf("tests: no registered subsystems\n");
		return;
	}

	printf("tests: all");
	for (uint32_t i = 0; i < count; i++)
		printf(", %s", ktest_registered_name(i));
	putchar('\n');
}

static void kdebug_run_tests(const char* selector) {
	ktest_run_summary_t summary;
	if (!ktest_run(selector, &summary, kdebug_test_reporter, NULL)) {
		printf("tests: unknown selector '%s'\n", selector != NULL ? selector : "");
		kdebug_print_test_selectors();
		return;
	}

	const bool pass = summary.failed == 0;
	printf("tests: suites=%u passed=%u failed=%u checks=%u check_failures=%u -> %s\n",
		summary.executed, summary.passed, summary.failed,
		summary.checks, summary.check_failures,
		pass ? "PASS" : "FAIL");
}

static void kdebug_register_test_hook(const char* name, ktest_hook_t hook) {
	if (!ktest_register_subsystem(name, hook))
		printf("kdebug: failed to register test hook '%s'\n", name);
}

static void kdebug_register_tests(void) {
	if (tests_registered)
		return;

	kdebug_register_test_hook("memmap", kdebug_test_memmap);
	kdebug_register_test_hook("vma", kdebug_test_vma_tree);
	kdebug_register_test_hook("slab", kdebug_test_slab);
	kdebug_register_test_hook("pmm", kdebug_test_pmm_numa);
	kdebug_register_test_hook("aspace", kdebug_test_address_spaces);
	kdebug_register_test_hook("sched", kdebug_test_scheduler);
	kdebug_register_test_hook("sync", kdebug_test_sync_primitives);
	kdebug_register_test_hook("panic", kdebug_test_panic_log);
	tests_registered = true;
}

static void kdebug_execute(char* line) {
	if (strcmp(line, "help") == 0) {
		printf("commands: help, tasks, cpus, mem, numa, schedstat [reset], ls, cat <file>, test [all|<subsystem>|list], paniclog [clear], continue\n");
	} else if (strcmp(line, "tasks") == 0) {
		kdebug_show_tasks();
	} else if (strcmp(line, "cpus") == 0) {
		kdebug_show_cpus();
	} else if (strcmp(line, "mem") == 0) {
		printf("memory: total=%u KiB free=%u KiB numa_nodes=%u\n",
			pmm_total_memory_kib(), pmm_free_memory_kib(), (unsigned) pmm_numa_node_count());
	} else if (strcmp(line, "numa") == 0) {
		kdebug_show_numa();
	} else if (strcmp(line, "schedstat") == 0) {
		kdebug_show_schedstat();
	} else if (strcmp(line, "schedstat reset") == 0) {
		scheduler_trace_reset();
		printf("schedstat: reset\n");
	} else if (strcmp(line, "ls") == 0) {
		kdebug_list_files();
	} else if (starts_with(line, "cat ")) {
		kdebug_cat_file(line + 4);
	} else if (strcmp(line, "test") == 0) {
		kdebug_run_tests("all");
	} else if (strcmp(line, "test list") == 0) {
		kdebug_print_test_selectors();
	} else if (starts_with(line, "test ")) {
		kdebug_run_tests(line + 5);
	} else if (strcmp(line, "paniclog") == 0) {
		kdebug_show_panic_log();
	} else if (strcmp(line, "paniclog clear") == 0) {
		panic_log_clear();
		printf("paniclog: cleared\n");
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
	kdebug_register_tests();
	debugger_active = false;
	scheduler_create_kernel_task("kdebug", kdebug_task, NULL);
}
