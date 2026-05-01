#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/cpu.h>
#include <kernel/gdt.h>
#include <kernel/interrupts.h>
#include <kernel/paging.h>
#include <kernel/panic.h>
#include <kernel/pmm.h>
#include <kernel/scheduler.h>
#include <kernel/x86.h>

#define SCHEDULER_YIELD_VECTOR          48u
#define TASK_STACK_SIZE                 16384u
#define TASK_STACK_PAGES                (TASK_STACK_SIZE / PAGE_SIZE)
#define SCHEDULER_INVALID_CPU_ID        0xFFu
#define SCHEDULER_BALANCE_INTERVAL_TICKS 16u
#define SCHEDULER_BALANCE_MAX_MIGRATIONS 2u
#define SCHEDULER_U64_MAX               ((uint64_t) (~0ull))

typedef enum task_state {
	TASK_UNUSED = 0,
	TASK_RUNNABLE,
	TASK_RUNNING,
	TASK_SLEEPING,
	TASK_BLOCKED,
	TASK_ZOMBIE
} task_state_t;

typedef enum task_list_kind {
	TASK_LIST_NONE = 0,
	TASK_LIST_FREE,
	TASK_LIST_RUNNABLE,
	TASK_LIST_SLEEPING,
	TASK_LIST_BLOCKED,
	TASK_LIST_ZOMBIE
} task_list_kind_t;

typedef enum scheduler_enqueue_reason {
	SCHEDULER_ENQUEUE_REASON_RESCHEDULE = 0,
	SCHEDULER_ENQUEUE_REASON_WAKEUP
} scheduler_enqueue_reason_t;

typedef struct task task_t;

struct task {
	uint32_t id;
	const char* name;
	task_state_t state;
	scheduler_priority_t priority;
	uint32_t cpu_affinity_mask;
	uint8_t preferred_numa_node;
	uint8_t assigned_cpu;
	uint8_t assigned_numa_node;
	interrupt_frame_t* frame;
	void* stack_base;
	uint32_t kernel_stack_top;
	uint32_t wakeup_tick;
	uint32_t runnable_enqueue_tick;
	kernel_task_entry_t entry;
	void* arg;
	paging_space_t* space;
	const void* wait_channel;
	bool wait_result;
	uint32_t ticks_in_slice;
	task_t* list_prev;
	task_t* list_next;
	task_list_kind_t list_kind;
};

typedef struct task_list {
	task_t* head;
	task_t* tail;
	uint32_t count;
} task_list_t;

typedef struct scheduler_cpu_state {
	uint8_t id;
	uint8_t numa_node;
	bool online;
	task_list_t runnable[SCHEDULER_PRIORITY_COUNT];
	uint32_t runnable_count;
	task_t* current;
} scheduler_cpu_state_t;

static task_t tasks[SCHEDULER_MAX_THREADS];
static scheduler_cpu_state_t cpus[SCHEDULER_MAX_CPUS];
static task_list_t free_list;
static task_list_t sleeping_list;
static task_list_t blocked_list;
static task_list_t zombie_list;

static uint8_t configured_cpu_count;
static uint8_t configured_node_count;
static uint8_t boot_cpu_id;
static uint32_t system_ticks;
static uint32_t next_task_id;
static uint32_t last_balance_tick;
static bool scheduler_bootstrapped;
static volatile uint32_t scheduler_state_lock;
static scheduler_trace_counters_t scheduler_trace_counters;
static scheduler_trace_latency_t scheduler_trace_latency;
static uint8_t scheduler_join_wait_channel_token;

typedef struct scheduler_reap_resources {
	void* stack_base;
	paging_space_t* space;
} scheduler_reap_resources_t;

static const uint8_t priority_time_slice[SCHEDULER_PRIORITY_COUNT] = {
	2u, 3u, 5u, 7u, 9u
};

static uint32_t scheduler_lock_irqsave(void);
static void scheduler_unlock_irqrestore(uint32_t irq_flags);
static void task_list_remove(task_list_t* list, task_t* task);
static void scheduler_release_task_slot(task_t* task);
static task_t* scheduler_find_task_by_id(uint32_t task_id);

static void scheduler_trace_counter_inc(uint64_t* counter) {
	if (counter == NULL || *counter == SCHEDULER_U64_MAX)
		return;
	(*counter)++;
}

static void scheduler_trace_latency_reset_metric(scheduler_trace_latency_metric_t* metric) {
	if (metric == NULL)
		return;

	metric->sample_count = 0;
	metric->total_ticks = 0;
	metric->min_ticks = 0;
	metric->max_ticks = 0;
}

static void scheduler_trace_latency_record(
	scheduler_trace_latency_metric_t* metric, uint32_t ticks) {
	if (metric == NULL)
		return;

	if (metric->sample_count == 0 || ticks < metric->min_ticks)
		metric->min_ticks = ticks;
	if (ticks > metric->max_ticks)
		metric->max_ticks = ticks;

	if (metric->sample_count < SCHEDULER_U64_MAX)
		metric->sample_count++;

	if (metric->total_ticks > SCHEDULER_U64_MAX - (uint64_t) ticks)
		metric->total_ticks = SCHEDULER_U64_MAX;
	else
		metric->total_ticks += (uint64_t) ticks;
}

static void scheduler_trace_reset_locked(void) {
	scheduler_trace_counters.timer_ticks = 0;
	scheduler_trace_counters.schedule_events = 0;
	scheduler_trace_counters.context_switches = 0;
	scheduler_trace_counters.preemptions = 0;
	scheduler_trace_counters.voluntary_yields = 0;
	scheduler_trace_counters.wakeups = 0;
	scheduler_trace_counters.wait_calls = 0;
	scheduler_trace_counters.wait_timeouts = 0;
	scheduler_trace_counters.sleep_calls = 0;
	scheduler_trace_counters.task_creations = 0;
	scheduler_trace_counters.task_exits = 0;
	scheduler_trace_counters.priority_updates = 0;
	scheduler_trace_counters.load_balance_runs = 0;
	scheduler_trace_counters.load_balance_migrations = 0;
	scheduler_trace_counters.task_migrations = 0;
	scheduler_trace_counters.remote_wakeup_ipis = 0;
	scheduler_trace_counters.remote_reschedule_ipis = 0;

	scheduler_trace_latency_reset_metric(&scheduler_trace_latency.runnable_wait_ticks);
	scheduler_trace_latency_reset_metric(&scheduler_trace_latency.sleep_overshoot_ticks);
	scheduler_trace_latency_reset_metric(&scheduler_trace_latency.wait_timeout_overshoot_ticks);
}

static void scheduler_reap_resources_reset(scheduler_reap_resources_t* resources) {
	if (resources == NULL)
		return;

	resources->stack_base = NULL;
	resources->space = NULL;
}

static bool scheduler_task_is_current_on_any_cpu_locked(const task_t* task) {
	if (task == NULL)
		return false;

	for (uint32_t cpu = 0; cpu < configured_cpu_count; cpu++) {
		if (!cpus[cpu].online)
			continue;
		if (cpus[cpu].current == task)
			return true;
	}

	return false;
}

static bool scheduler_detach_zombie_task_locked(
	task_t* task, scheduler_reap_resources_t* resources) {
	if (task == NULL || resources == NULL)
		return false;
	if (task->state != TASK_ZOMBIE || task->list_kind != TASK_LIST_ZOMBIE)
		return false;
	if (scheduler_task_is_current_on_any_cpu_locked(task))
		return false;

	task_list_remove(&zombie_list, task);
	resources->stack_base = task->stack_base;
	resources->space = task->space;
	task->stack_base = NULL;
	task->space = NULL;
	scheduler_release_task_slot(task);
	return true;
}

static void scheduler_cleanup_reaped_resources(scheduler_reap_resources_t* resources) {
	if (resources == NULL)
		return;

	if (resources->stack_base != NULL)
		paging_free_pages(resources->stack_base, TASK_STACK_PAGES);

	if (resources->space != NULL && resources->space != paging_kernel_space())
		paging_destroy_process_space(resources->space);

	scheduler_reap_resources_reset(resources);
}

static const char* task_state_name(task_state_t state) {
	switch (state) {
	case TASK_UNUSED:
		return "unused";
	case TASK_RUNNABLE:
		return "runnable";
	case TASK_RUNNING:
		return "running";
	case TASK_SLEEPING:
		return "sleeping";
	case TASK_BLOCKED:
		return "blocked";
	case TASK_ZOMBIE:
		return "zombie";
	}

	return "unknown";
}

static scheduler_priority_t scheduler_priority_sanitize(scheduler_priority_t priority) {
	if ((uint32_t) priority >= (uint32_t) SCHEDULER_PRIORITY_COUNT)
		return SCHEDULER_PRIORITY_NORMAL;
	return priority;
}

static uint8_t scheduler_priority_timeslice(scheduler_priority_t priority) {
	const scheduler_priority_t safe_priority = scheduler_priority_sanitize(priority);
	return priority_time_slice[(uint32_t) safe_priority];
}

static void task_list_init(task_list_t* list) {
	list->head = NULL;
	list->tail = NULL;
	list->count = 0;
}

static void task_list_push_back(task_list_t* list, task_t* task, task_list_kind_t kind) {
	task->list_prev = list->tail;
	task->list_next = NULL;

	if (list->tail != NULL)
		list->tail->list_next = task;
	else
		list->head = task;

	list->tail = task;
	list->count++;
	task->list_kind = kind;
}

static void task_list_remove(task_list_t* list, task_t* task) {
	if (task->list_prev != NULL)
		task->list_prev->list_next = task->list_next;
	else
		list->head = task->list_next;

	if (task->list_next != NULL)
		task->list_next->list_prev = task->list_prev;
	else
		list->tail = task->list_prev;

	if (list->count > 0)
		list->count--;

	task->list_prev = NULL;
	task->list_next = NULL;
	task->list_kind = TASK_LIST_NONE;
}

static task_t* task_list_pop_front(task_list_t* list) {
	task_t* task = list->head;
	if (task == NULL)
		return NULL;

	task_list_remove(list, task);
	return task;
}

static void task_reset(task_t* task) {
	task->id = 0;
	task->name = NULL;
	task->state = TASK_UNUSED;
	task->priority = SCHEDULER_PRIORITY_NORMAL;
	task->cpu_affinity_mask = SCHEDULER_CPU_MASK_ALL;
	task->preferred_numa_node = SCHEDULER_NUMA_NODE_ANY;
	task->assigned_cpu = boot_cpu_id;
	task->assigned_numa_node = 0;
	task->frame = NULL;
	task->stack_base = NULL;
	task->kernel_stack_top = 0;
	task->wakeup_tick = 0;
	task->runnable_enqueue_tick = 0;
	task->entry = NULL;
	task->arg = NULL;
	task->space = NULL;
	task->wait_channel = NULL;
	task->wait_result = false;
	task->ticks_in_slice = 0;
	task->list_prev = NULL;
	task->list_next = NULL;
	task->list_kind = TASK_LIST_NONE;
}

static void scheduler_cpu_state_init(
	scheduler_cpu_state_t* cpu, uint8_t id, uint8_t node, bool online) {
	cpu->id = id;
	cpu->numa_node = node;
	cpu->online = online;
	cpu->runnable_count = 0;
	cpu->current = NULL;

	for (uint32_t i = 0; i < (uint32_t) SCHEDULER_PRIORITY_COUNT; i++)
		task_list_init(&cpu->runnable[i]);
}

static bool scheduler_has_live_tasks(void) {
	for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
		if (tasks[i].state != TASK_UNUSED)
			return true;
	}

	return false;
}

void scheduler_topology_default(scheduler_topology_config_t* config) {
	if (config == NULL)
		return;

	config->cpu_count = 1;
	config->boot_cpu_id = 0;
	config->node_count = 1;

	for (uint32_t i = 0; i < SCHEDULER_MAX_CPUS; i++) {
		config->cpu_to_node[i] = 0;
		config->cpu_online[i] = i == 0;
	}
}

bool scheduler_configure_topology(const scheduler_topology_config_t* config) {
	if (config == NULL)
		return false;

	const uint32_t irq_flags = scheduler_lock_irqsave();
	bool success = false;

	if (scheduler_bootstrapped || scheduler_has_live_tasks())
		goto done;

	if (config->cpu_count == 0 || config->cpu_count > SCHEDULER_MAX_CPUS)
		goto done;

	if (config->node_count == 0 || config->node_count > SCHEDULER_MAX_NUMA_NODES)
		goto done;

	if (config->boot_cpu_id >= config->cpu_count)
		goto done;

	uint32_t online_count = 0;
	for (uint32_t i = 0; i < config->cpu_count; i++) {
		if (config->cpu_to_node[i] >= config->node_count)
			goto done;
		if (config->cpu_online[i])
			online_count++;
	}

	if (online_count == 0 || !config->cpu_online[config->boot_cpu_id])
		goto done;

	configured_cpu_count = config->cpu_count;
	configured_node_count = config->node_count;
	boot_cpu_id = config->boot_cpu_id;

	for (uint32_t i = 0; i < SCHEDULER_MAX_CPUS; i++) {
		bool online = false;
		uint8_t node = 0;
		if (i < configured_cpu_count) {
			online = config->cpu_online[i];
			node = config->cpu_to_node[i];
		}

		scheduler_cpu_state_init(&cpus[i], (uint8_t) i, node, online);
	}

	if (!pmm_configure_numa_topology(configured_node_count))
		goto done;

	success = true;

done:
	scheduler_unlock_irqrestore(irq_flags);
	return success;
}

static uint32_t scheduler_online_cpu_mask(void) {
	uint32_t mask = 0;

	for (uint32_t i = 0; i < configured_cpu_count; i++) {
		if (cpus[i].online)
			mask |= (1u << i);
	}

	return mask;
}

static uint32_t scheduler_normalize_affinity_mask(uint32_t affinity_mask) {
	uint32_t normalized = affinity_mask & scheduler_online_cpu_mask();
	if (normalized == 0)
		normalized = scheduler_online_cpu_mask();
	return normalized;
}

static scheduler_cpu_state_t* scheduler_cpu_by_id(uint8_t cpu_id) {
	if (cpu_id >= configured_cpu_count || !cpus[cpu_id].online)
		return &cpus[boot_cpu_id];
	return &cpus[cpu_id];
}

static scheduler_cpu_state_t* scheduler_current_cpu_state(void) {
	if (configured_cpu_count == 0)
		return &cpus[0];

	uint32_t cpu_id = cpu_current_id();
	if (cpu_id >= configured_cpu_count || !cpus[cpu_id].online)
		cpu_id = boot_cpu_id;
	return &cpus[cpu_id];
}

uint32_t scheduler_current_cpu(void) {
	return scheduler_current_cpu_state()->id;
}

static task_t* scheduler_current_task(void) {
	return scheduler_current_cpu_state()->current;
}

static uint32_t scheduler_irq_save(void) {
	const uint32_t flags = x86_read_eflags();
	x86_cli();
	return flags;
}

static void scheduler_irq_restore(uint32_t flags) {
	if ((flags & (1u << 9)) != 0)
		x86_sti();
}

static bool scheduler_atomic_try_lock_u32(volatile uint32_t* target) {
	uint32_t desired = 1u;
	asm volatile("xchgl %0, %1"
		: "+r"(desired), "+m"(*target)
		:
		: "memory");
	return desired == 0u;
}

static uint32_t scheduler_lock_irqsave(void) {
	const uint32_t irq_flags = scheduler_irq_save();

	for (;;) {
		if (scheduler_atomic_try_lock_u32(&scheduler_state_lock))
			return irq_flags;

		while (scheduler_state_lock != 0u)
			x86_pause();
	}
}

static void scheduler_unlock_irqrestore(uint32_t irq_flags) {
	asm volatile("" : : : "memory");
	scheduler_state_lock = 0;
	scheduler_irq_restore(irq_flags);
}

static uint32_t scheduler_cpu_load(const scheduler_cpu_state_t* cpu) {
	uint32_t load = cpu->runnable_count;
	if (cpu->current != NULL && cpu->current->state == TASK_RUNNING)
		load++;
	return load;
}

static uint8_t scheduler_select_best_cpu_from_view(
	uint8_t cpu_count,
	const bool* cpu_online,
	const uint8_t* cpu_to_node,
	const uint32_t* cpu_load,
	uint32_t affinity_mask,
	uint8_t preferred_numa_node,
	uint8_t local_cpu) {
	if (cpu_count == 0)
		return SCHEDULER_INVALID_CPU_ID;

	uint32_t online_mask = 0;
	for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
		if (cpu_online[cpu])
			online_mask |= (1u << cpu);
	}

	if (online_mask == 0)
		return SCHEDULER_INVALID_CPU_ID;

	uint32_t normalized_affinity = affinity_mask & online_mask;
	if (normalized_affinity == 0)
		normalized_affinity = online_mask;

	for (uint32_t pass = 0; pass < 2; pass++) {
		const bool enforce_preferred_node =
			pass == 0 && preferred_numa_node != SCHEDULER_NUMA_NODE_ANY;

		uint8_t best_cpu = SCHEDULER_INVALID_CPU_ID;
		uint32_t best_load = UINT_MAX;

		for (uint32_t cpu = 0; cpu < cpu_count; cpu++) {
			if ((normalized_affinity & (1u << cpu)) == 0)
				continue;

			if (enforce_preferred_node && cpu_to_node[cpu] != preferred_numa_node)
				continue;

			const uint32_t load = cpu_load[cpu];
			if (best_cpu == SCHEDULER_INVALID_CPU_ID || load < best_load) {
				best_cpu = (uint8_t) cpu;
				best_load = load;
				continue;
			}

			if (load == best_load) {
				if (cpu == local_cpu && best_cpu != local_cpu) {
					best_cpu = (uint8_t) cpu;
					continue;
				}

				if (best_cpu != local_cpu && cpu < best_cpu)
					best_cpu = (uint8_t) cpu;
			}
		}

		if (best_cpu != SCHEDULER_INVALID_CPU_ID)
			return best_cpu;
	}

	return SCHEDULER_INVALID_CPU_ID;
}

static uint8_t scheduler_select_target_cpu(uint32_t affinity_mask, uint8_t preferred_numa_node) {
	bool cpu_online[SCHEDULER_MAX_CPUS];
	uint8_t cpu_to_node[SCHEDULER_MAX_CPUS];
	uint32_t cpu_load[SCHEDULER_MAX_CPUS];
	uint8_t local_cpu = boot_cpu_id;

	if (scheduler_bootstrapped)
		local_cpu = (uint8_t) scheduler_current_cpu();

	for (uint32_t cpu = 0; cpu < configured_cpu_count; cpu++) {
		cpu_online[cpu] = cpus[cpu].online;
		cpu_to_node[cpu] = cpus[cpu].numa_node;
		cpu_load[cpu] = scheduler_cpu_load(&cpus[cpu]);
	}

	uint8_t selected = scheduler_select_best_cpu_from_view(
		configured_cpu_count, cpu_online, cpu_to_node, cpu_load,
		affinity_mask, preferred_numa_node, local_cpu);

	if (selected == SCHEDULER_INVALID_CPU_ID)
		selected = boot_cpu_id;

	return selected;
}

static void scheduler_notify_remote_runnable(
	const scheduler_cpu_state_t* cpu, scheduler_enqueue_reason_t reason) {
	if (!scheduler_bootstrapped || cpu == NULL || !cpu->online || !cpu_smp_available())
		return;

	const uint32_t current_cpu = scheduler_current_cpu();
	if (cpu->id == current_cpu)
		return;

	if (reason == SCHEDULER_ENQUEUE_REASON_WAKEUP) {
		scheduler_trace_counter_inc(&scheduler_trace_counters.remote_wakeup_ipis);
		cpu_send_wakeup_ipi(cpu->id);
	} else {
		scheduler_trace_counter_inc(&scheduler_trace_counters.remote_reschedule_ipis);
		cpu_send_reschedule_ipi(cpu->id);
	}
}

static void scheduler_cpu_enqueue_runnable_local(scheduler_cpu_state_t* cpu, task_t* task) {
	const scheduler_priority_t priority = scheduler_priority_sanitize(task->priority);
	task->priority = priority;
	task->assigned_cpu = cpu->id;
	task->assigned_numa_node = cpu->numa_node;
	task->state = TASK_RUNNABLE;
	task->ticks_in_slice = 0;
	task->runnable_enqueue_tick = system_ticks;
	task_list_push_back(&cpu->runnable[(uint32_t) priority], task, TASK_LIST_RUNNABLE);
	cpu->runnable_count++;
}

static void scheduler_cpu_enqueue_runnable(
	scheduler_cpu_state_t* cpu, task_t* task, scheduler_enqueue_reason_t reason) {
	scheduler_cpu_enqueue_runnable_local(cpu, task);
	scheduler_notify_remote_runnable(cpu, reason);
}

static void scheduler_enqueue_runnable(task_t* task, scheduler_enqueue_reason_t reason) {
	scheduler_cpu_state_t* cpu = scheduler_cpu_by_id(task->assigned_cpu);
	scheduler_cpu_enqueue_runnable(cpu, task, reason);
}

static void scheduler_remove_runnable(task_t* task) {
	if (task == NULL || task->list_kind != TASK_LIST_RUNNABLE || task->state != TASK_RUNNABLE)
		return;

	scheduler_cpu_state_t* cpu = scheduler_cpu_by_id(task->assigned_cpu);
	const scheduler_priority_t priority = scheduler_priority_sanitize(task->priority);
	task_list_remove(&cpu->runnable[(uint32_t) priority], task);
	if (cpu->runnable_count > 0)
		cpu->runnable_count--;
}

static task_t* scheduler_cpu_dequeue_next_runnable(scheduler_cpu_state_t* cpu) {
	for (uint32_t priority = 0; priority < (uint32_t) SCHEDULER_PRIORITY_COUNT; priority++) {
		task_t* next = task_list_pop_front(&cpu->runnable[priority]);
		if (next != NULL) {
			if (cpu->runnable_count > 0)
				cpu->runnable_count--;
			scheduler_trace_latency_record(
				&scheduler_trace_latency.runnable_wait_ticks,
				system_ticks - next->runnable_enqueue_tick);
			return next;
		}
	}

	return NULL;
}

static bool scheduler_migrate_runnable_task(
	scheduler_cpu_state_t* source_cpu,
	scheduler_cpu_state_t* target_cpu,
	task_t* task,
	bool notify_target) {
	if (source_cpu == NULL || target_cpu == NULL || task == NULL || source_cpu == target_cpu)
		return false;
	if (!source_cpu->online || !target_cpu->online)
		return false;
	if (task->state != TASK_RUNNABLE || task->list_kind != TASK_LIST_RUNNABLE)
		return false;
	if (task->assigned_cpu != source_cpu->id)
		return false;
	if ((task->cpu_affinity_mask & (1u << target_cpu->id)) == 0)
		return false;

	const scheduler_priority_t priority = scheduler_priority_sanitize(task->priority);
	task_list_remove(&source_cpu->runnable[(uint32_t) priority], task);
	if (source_cpu->runnable_count > 0)
		source_cpu->runnable_count--;

	if (notify_target)
		scheduler_cpu_enqueue_runnable(target_cpu, task, SCHEDULER_ENQUEUE_REASON_RESCHEDULE);
	else
		scheduler_cpu_enqueue_runnable_local(target_cpu, task);

	scheduler_trace_counter_inc(&scheduler_trace_counters.task_migrations);
	return true;
}

static bool scheduler_balance_once_from_view(
	scheduler_cpu_state_t* cpu_view,
	uint8_t cpu_count,
	const bool* cpu_online,
	const uint8_t* cpu_to_node,
	uint32_t* cpu_load,
	bool notify_target) {
	if (cpu_view == NULL || cpu_online == NULL || cpu_to_node == NULL || cpu_load == NULL)
		return false;
	if (cpu_count <= 1)
		return false;

	for (uint32_t source_index = 0; source_index < cpu_count; source_index++) {
		scheduler_cpu_state_t* source_cpu = &cpu_view[source_index];
		if (!cpu_online[source_index] || !source_cpu->online || source_cpu->runnable_count == 0)
			continue;

		for (uint32_t priority_index = (uint32_t) SCHEDULER_PRIORITY_COUNT;
			priority_index > 0;
			priority_index--) {
			task_t* task = source_cpu->runnable[priority_index - 1u].head;
			while (task != NULL) {
				task_t* next = task->list_next;
				const uint8_t target_id = scheduler_select_best_cpu_from_view(
					cpu_count, cpu_online, cpu_to_node, cpu_load,
					task->cpu_affinity_mask, task->preferred_numa_node, source_cpu->id);
				if (target_id == SCHEDULER_INVALID_CPU_ID || target_id == source_cpu->id) {
					task = next;
					continue;
				}

				if (cpu_load[source_index] <= cpu_load[target_id] + 1u) {
					task = next;
					continue;
				}

				scheduler_cpu_state_t* target_cpu = &cpu_view[target_id];
				if (!cpu_online[target_id] || !target_cpu->online) {
					task = next;
					continue;
				}

				if (!scheduler_migrate_runnable_task(source_cpu, target_cpu, task, notify_target)) {
					task = next;
					continue;
				}

				cpu_load[source_index]--;
				cpu_load[target_id]++;
				return true;
			}
		}
	}

	return false;
}

static bool scheduler_should_balance_now(void) {
	if (!scheduler_bootstrapped || configured_cpu_count <= 1 || !cpu_smp_available())
		return false;
	if ((system_ticks - last_balance_tick) < SCHEDULER_BALANCE_INTERVAL_TICKS)
		return false;

	last_balance_tick = system_ticks;
	return true;
}

static void scheduler_balance_periodic(void) {
	bool cpu_online[SCHEDULER_MAX_CPUS];
	uint8_t cpu_to_node[SCHEDULER_MAX_CPUS];
	uint32_t cpu_load[SCHEDULER_MAX_CPUS];
	scheduler_trace_counter_inc(&scheduler_trace_counters.load_balance_runs);

	for (uint32_t cpu = 0; cpu < configured_cpu_count; cpu++) {
		cpu_online[cpu] = cpus[cpu].online;
		cpu_to_node[cpu] = cpus[cpu].numa_node;
		cpu_load[cpu] = scheduler_cpu_load(&cpus[cpu]);
	}

	for (uint32_t migrated = 0; migrated < SCHEDULER_BALANCE_MAX_MIGRATIONS; migrated++) {
		if (!scheduler_balance_once_from_view(cpus, configured_cpu_count,
			cpu_online, cpu_to_node, cpu_load, true))
			break;
		scheduler_trace_counter_inc(&scheduler_trace_counters.load_balance_migrations);
	}
}

static bool scheduler_cpu_has_higher_priority_runnable(
	const scheduler_cpu_state_t* cpu, scheduler_priority_t priority) {
	const scheduler_priority_t safe_priority = scheduler_priority_sanitize(priority);
	for (uint32_t i = 0; i < (uint32_t) safe_priority; i++) {
		if (cpu->runnable[i].count > 0)
			return true;
	}

	return false;
}

static void scheduler_advance_timers(void) {
	task_t* task = sleeping_list.head;
	while (task != NULL) {
		task_t* next = task->list_next;
		if (task->wakeup_tick <= system_ticks) {
			scheduler_trace_latency_record(
				&scheduler_trace_latency.sleep_overshoot_ticks,
				system_ticks - task->wakeup_tick);
			task_list_remove(&sleeping_list, task);
			scheduler_enqueue_runnable(task, SCHEDULER_ENQUEUE_REASON_WAKEUP);
			scheduler_trace_counter_inc(&scheduler_trace_counters.wakeups);
		}
		task = next;
	}

	task = blocked_list.head;
	while (task != NULL) {
		task_t* next = task->list_next;
		if (task->wakeup_tick != 0 && task->wakeup_tick <= system_ticks) {
			scheduler_trace_latency_record(
				&scheduler_trace_latency.wait_timeout_overshoot_ticks,
				system_ticks - task->wakeup_tick);
			task_list_remove(&blocked_list, task);
			task->wait_channel = NULL;
			task->wait_result = false;
			task->wakeup_tick = 0;
			scheduler_enqueue_runnable(task, SCHEDULER_ENQUEUE_REASON_WAKEUP);
			scheduler_trace_counter_inc(&scheduler_trace_counters.wakeups);
			scheduler_trace_counter_inc(&scheduler_trace_counters.wait_timeouts);
		}
		task = next;
	}
}

static interrupt_frame_t* scheduler_switch_to_task(
	scheduler_cpu_state_t* cpu, task_t* next, interrupt_frame_t* fallback_frame) {
	if (next == NULL) {
		task_t* current = cpu->current;
		if (current != NULL && current->state == TASK_RUNNING) {
			gdt_set_kernel_stack(current->kernel_stack_top);
			paging_switch_space(current->space);
			return fallback_frame;
		}

		panic("scheduler: cpu %u has no runnable tasks", cpu->id);
	}

	task_t* previous = cpu->current;
	next->state = TASK_RUNNING;
	next->ticks_in_slice = 0;
	cpu->current = next;
	if (previous != next)
		scheduler_trace_counter_inc(&scheduler_trace_counters.context_switches);
	gdt_set_kernel_stack(next->kernel_stack_top);
	paging_switch_space(next->space);
	return next->frame;
}

static interrupt_frame_t* scheduler_schedule_cpu(
	scheduler_cpu_state_t* cpu, interrupt_frame_t* frame, bool force_switch) {
	task_t* current = cpu->current;
	scheduler_trace_counter_inc(&scheduler_trace_counters.schedule_events);

	if (current != NULL)
		current->frame = frame;

	if (current != NULL && current->state == TASK_RUNNING && !force_switch) {
		if (!scheduler_cpu_has_higher_priority_runnable(cpu, current->priority))
			return frame;
		force_switch = true;
	}

	if (current != NULL && current->state == TASK_RUNNING && force_switch)
		scheduler_cpu_enqueue_runnable(cpu, current, SCHEDULER_ENQUEUE_REASON_RESCHEDULE);

	task_t* next = scheduler_cpu_dequeue_next_runnable(cpu);
	return scheduler_switch_to_task(cpu, next, frame);
}

static task_t* scheduler_allocate_task_slot(void) {
	task_t* task = task_list_pop_front(&free_list);
	if (task == NULL)
		return NULL;

	task_reset(task);
	return task;
}

static void scheduler_release_task_slot(task_t* task) {
	if (task == NULL)
		return;

	task_reset(task);
	task_list_push_back(&free_list, task, TASK_LIST_FREE);
}

static void task_trampoline(void) {
	task_t* task = scheduler_current_task();
	if (task == NULL || task->entry == NULL)
		panic("scheduler entered task trampoline without a valid task");

	task->entry(task->arg);
	scheduler_exit_current();
}

void scheduler_task_config_default(scheduler_task_config_t* config) {
	if (config == NULL)
		return;

	config->priority = SCHEDULER_PRIORITY_NORMAL;
	config->cpu_affinity_mask = SCHEDULER_CPU_MASK_ALL;
	config->preferred_numa_node = SCHEDULER_NUMA_NODE_ANY;
}

void scheduler_init(void) {
	task_list_init(&free_list);
	task_list_init(&sleeping_list);
	task_list_init(&blocked_list);
	task_list_init(&zombie_list);

	scheduler_bootstrapped = false;
	system_ticks = 0;
	next_task_id = 1;
	last_balance_tick = 0;
	configured_cpu_count = 0;
	configured_node_count = 0;
	boot_cpu_id = 0;
	scheduler_state_lock = 0;
	scheduler_trace_reset_locked();

	for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
		task_reset(&tasks[i]);
		task_list_push_back(&free_list, &tasks[i], TASK_LIST_FREE);
	}

	scheduler_topology_config_t topology;
	scheduler_topology_default(&topology);
	if (!scheduler_configure_topology(&topology))
		panic("scheduler failed to configure default topology");

	register_interrupt_handler(SCHEDULER_YIELD_VECTOR, scheduler_on_yield);
}

static task_t* scheduler_bootstrap_cpu_task(
	uint8_t cpu_id, uint32_t kernel_stack_top, const char* name) {
	if (configured_cpu_count == 0 || cpu_id >= configured_cpu_count || !cpus[cpu_id].online)
		return NULL;

	scheduler_cpu_state_t* cpu = &cpus[cpu_id];
	if (cpu->current != NULL)
		return NULL;

	task_t* task = scheduler_allocate_task_slot();
	if (task == NULL)
		return NULL;

	task->id = next_task_id++;
	task->name = name != NULL ? name : "bootstrap";
	task->state = TASK_RUNNING;
	task->priority = SCHEDULER_PRIORITY_NORMAL;
	task->cpu_affinity_mask = scheduler_online_cpu_mask();
	task->preferred_numa_node = SCHEDULER_NUMA_NODE_ANY;
	task->assigned_cpu = cpu->id;
	task->assigned_numa_node = cpu->numa_node;
	task->frame = NULL;
	task->stack_base = NULL;
	task->kernel_stack_top = kernel_stack_top;
	task->wakeup_tick = 0;
	task->runnable_enqueue_tick = 0;
	task->entry = NULL;
	task->arg = NULL;
	task->space = paging_kernel_space();
	task->ticks_in_slice = 0;
	task->wait_channel = NULL;
	task->wait_result = false;

	cpu->current = task;
	return task;
}

void scheduler_bootstrap_current(const char* name) {
	const uint32_t irq_flags = scheduler_lock_irqsave();
	if (scheduler_bootstrapped)
		panic("scheduler bootstrap attempted twice");
	if (configured_cpu_count == 0)
		panic("scheduler topology is not configured");

	uint32_t bootstrap_stack_top;
	asm volatile("mov %%esp, %0" : "=r"(bootstrap_stack_top));

	const char* task_name = name != NULL ? name : "bootstrap";
	task_t* task = scheduler_bootstrap_cpu_task(boot_cpu_id, bootstrap_stack_top, task_name);
	if (task == NULL)
		panic("scheduler could not attach bootstrap task to cpu %u", boot_cpu_id);

	task->space = paging_current_space();
	scheduler_bootstrapped = true;
	gdt_set_kernel_stack(task->kernel_stack_top);
	scheduler_unlock_irqrestore(irq_flags);
}

bool scheduler_bootstrap_secondary_current(
	uint32_t cpu_id, uint32_t kernel_stack_top, const char* name) {
	const uint32_t irq_flags = scheduler_lock_irqsave();
	if (!scheduler_bootstrapped)
		goto fail;
	if (cpu_id >= configured_cpu_count)
		goto fail;
	if (cpu_id == boot_cpu_id)
		goto fail;

	task_t* task = scheduler_bootstrap_cpu_task((uint8_t) cpu_id, kernel_stack_top, name);
	scheduler_unlock_irqrestore(irq_flags);
	return task != NULL;

fail:
	scheduler_unlock_irqrestore(irq_flags);
	return false;
}

bool scheduler_create_kernel_task(const char* name, kernel_task_entry_t entry, void* arg) {
	scheduler_task_config_t config;
	scheduler_task_config_default(&config);
	return scheduler_create_kernel_task_ex(name, entry, arg, &config);
}

bool scheduler_create_kernel_task_ex(
	const char* name,
	kernel_task_entry_t entry,
	void* arg,
	const scheduler_task_config_t* config) {
	if (entry == NULL)
		return false;

	scheduler_task_config_t effective;
	scheduler_task_config_default(&effective);
	if (config != NULL)
		effective = *config;
	uint8_t preferred_memory_node = 0;

	const uint32_t reserve_irq_flags = scheduler_lock_irqsave();
	if (configured_cpu_count == 0) {
		scheduler_unlock_irqrestore(reserve_irq_flags);
		return false;
	}

	effective.priority = scheduler_priority_sanitize(effective.priority);
	effective.cpu_affinity_mask = scheduler_normalize_affinity_mask(effective.cpu_affinity_mask);
	if (effective.preferred_numa_node != SCHEDULER_NUMA_NODE_ANY &&
		effective.preferred_numa_node >= configured_node_count)
		effective.preferred_numa_node = SCHEDULER_NUMA_NODE_ANY;

	const uint8_t target_cpu_hint = scheduler_select_target_cpu(
		effective.cpu_affinity_mask, effective.preferred_numa_node);
	scheduler_cpu_state_t* hint_cpu = scheduler_cpu_by_id(target_cpu_hint);
	preferred_memory_node = hint_cpu->numa_node;

	task_t* task = scheduler_allocate_task_slot();
	scheduler_unlock_irqrestore(reserve_irq_flags);
	if (task == NULL)
		return false;

	paging_space_t* space = paging_create_process_space_on_node(preferred_memory_node);
	if (space == NULL) {
		const uint32_t release_irq_flags = scheduler_lock_irqsave();
		scheduler_release_task_slot(task);
		scheduler_unlock_irqrestore(release_irq_flags);
		return false;
	}

	uint8_t* stack = (uint8_t*) paging_alloc_pages_on_node(
		TASK_STACK_PAGES, preferred_memory_node);
	if (stack == NULL) {
		paging_destroy_process_space(space);
		const uint32_t release_irq_flags = scheduler_lock_irqsave();
		scheduler_release_task_slot(task);
		scheduler_unlock_irqrestore(release_irq_flags);
		return false;
	}

	interrupt_frame_t* frame =
		(interrupt_frame_t*) (stack + TASK_STACK_SIZE - sizeof(interrupt_frame_t));

	// Synthetic interrupt frame matching what the ISR epilogue restores with iret.
	frame->edi = 0;
	frame->esi = 0;
	frame->ebp = 0;
	frame->esp = 0;
	frame->ebx = 0;
	frame->edx = 0;
	frame->ecx = 0;
	frame->eax = 0;
	frame->vector = SCHEDULER_YIELD_VECTOR;
	frame->error_code = 0;
	frame->eip = (uint32_t) task_trampoline;
	frame->cs = GDT_SELECTOR_KERNEL_CODE;
	frame->eflags = 0x202;

	const uint32_t enqueue_irq_flags = scheduler_lock_irqsave();
	if (configured_cpu_count == 0) {
		scheduler_unlock_irqrestore(enqueue_irq_flags);
		paging_free_pages(stack, TASK_STACK_PAGES);
		paging_destroy_process_space(space);
		const uint32_t release_irq_flags = scheduler_lock_irqsave();
		scheduler_release_task_slot(task);
		scheduler_unlock_irqrestore(release_irq_flags);
		return false;
	}

	const uint8_t target_cpu = scheduler_select_target_cpu(
		effective.cpu_affinity_mask, effective.preferred_numa_node);
	scheduler_cpu_state_t* cpu = scheduler_cpu_by_id(target_cpu);

	task->id = next_task_id++;
	task->name = name != NULL ? name : "kernel-task";
	task->state = TASK_RUNNABLE;
	task->priority = effective.priority;
	task->cpu_affinity_mask = effective.cpu_affinity_mask;
	task->preferred_numa_node = effective.preferred_numa_node;
	task->assigned_cpu = cpu->id;
	task->assigned_numa_node = cpu->numa_node;
	task->frame = frame;
	task->stack_base = stack;
	task->kernel_stack_top = (uint32_t) (stack + TASK_STACK_SIZE);
	task->wakeup_tick = 0;
	task->runnable_enqueue_tick = 0;
	task->entry = entry;
	task->arg = arg;
	task->space = space;
	task->ticks_in_slice = 0;
	task->wait_channel = NULL;
	task->wait_result = false;

	scheduler_cpu_enqueue_runnable(cpu, task, SCHEDULER_ENQUEUE_REASON_RESCHEDULE);
	scheduler_trace_counter_inc(&scheduler_trace_counters.task_creations);
	scheduler_unlock_irqrestore(enqueue_irq_flags);
	return true;
}

static task_t* scheduler_find_task_by_id(uint32_t task_id) {
	for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
		if (tasks[i].state != TASK_UNUSED && tasks[i].id == task_id)
			return &tasks[i];
	}

	return NULL;
}

bool scheduler_set_task_priority(uint32_t task_id, scheduler_priority_t priority) {
	const uint32_t irq_flags = scheduler_lock_irqsave();
	task_t* task = scheduler_find_task_by_id(task_id);
	if (task == NULL) {
		scheduler_unlock_irqrestore(irq_flags);
		return false;
	}

	const scheduler_priority_t safe_priority = scheduler_priority_sanitize(priority);
	if (task->priority == safe_priority) {
		scheduler_unlock_irqrestore(irq_flags);
		return true;
	}

	if (task->state == TASK_RUNNABLE && task->list_kind == TASK_LIST_RUNNABLE)
		scheduler_remove_runnable(task);

	task->priority = safe_priority;

	if (task->state == TASK_RUNNABLE)
		scheduler_enqueue_runnable(task, SCHEDULER_ENQUEUE_REASON_RESCHEDULE);

	if (task->state == TASK_RUNNING)
		task->ticks_in_slice = 0;

	scheduler_trace_counter_inc(&scheduler_trace_counters.priority_updates);
	scheduler_unlock_irqrestore(irq_flags);
	return true;
}

interrupt_frame_t* scheduler_on_timer_tick(interrupt_frame_t* frame) {
	const uint32_t irq_flags = scheduler_lock_irqsave();
	system_ticks++;
	scheduler_trace_counter_inc(&scheduler_trace_counters.timer_ticks);
	scheduler_advance_timers();

	scheduler_cpu_state_t* cpu = scheduler_current_cpu_state();
	task_t* current = cpu->current;

	if (cpu->id == boot_cpu_id && scheduler_should_balance_now())
		scheduler_balance_periodic();

	if (current == NULL) {
		interrupt_frame_t* next = scheduler_schedule_cpu(cpu, frame, true);
		scheduler_unlock_irqrestore(irq_flags);
		return next;
	}

	if (current->state != TASK_RUNNING) {
		interrupt_frame_t* next = scheduler_schedule_cpu(cpu, frame, true);
		scheduler_unlock_irqrestore(irq_flags);
		return next;
	}

	current->ticks_in_slice++;
	const uint8_t slice = scheduler_priority_timeslice(current->priority);
	if (current->ticks_in_slice >= slice ||
		scheduler_cpu_has_higher_priority_runnable(cpu, current->priority)) {
		scheduler_trace_counter_inc(&scheduler_trace_counters.preemptions);
		interrupt_frame_t* next = scheduler_schedule_cpu(cpu, frame, true);
		scheduler_unlock_irqrestore(irq_flags);
		return next;
	}

	current->frame = frame;
	scheduler_unlock_irqrestore(irq_flags);
	return frame;
}

interrupt_frame_t* scheduler_on_yield(interrupt_frame_t* frame) {
	const uint32_t irq_flags = scheduler_lock_irqsave();
	scheduler_trace_counter_inc(&scheduler_trace_counters.voluntary_yields);
	scheduler_cpu_state_t* cpu = scheduler_current_cpu_state();
	interrupt_frame_t* next = scheduler_schedule_cpu(cpu, frame, true);
	scheduler_unlock_irqrestore(irq_flags);
	return next;
}

void scheduler_sleep(uint32_t ticks) {
	if (ticks == 0) {
		scheduler_yield();
		return;
	}

	task_t* task = scheduler_current_task();
	if (task == NULL)
		return;

	const uint32_t irq_flags = scheduler_lock_irqsave();
	scheduler_trace_counter_inc(&scheduler_trace_counters.sleep_calls);
	task->wakeup_tick = system_ticks + ticks;
	task->state = TASK_SLEEPING;
	task_list_push_back(&sleeping_list, task, TASK_LIST_SLEEPING);
	scheduler_unlock_irqrestore(irq_flags);
	asm volatile("int %0" : : "i"(SCHEDULER_YIELD_VECTOR));
}

bool scheduler_wait_channel(const void* wait_channel, uint32_t timeout_ticks) {
	if (wait_channel == NULL || timeout_ticks == 0)
		return false;

	task_t* task = scheduler_current_task();
	if (task == NULL)
		return false;

	const uint32_t irq_flags = scheduler_lock_irqsave();
	scheduler_trace_counter_inc(&scheduler_trace_counters.wait_calls);
	task->wait_channel = wait_channel;
	task->wait_result = false;
	if (timeout_ticks == SCHEDULER_WAIT_FOREVER)
		task->wakeup_tick = 0;
	else
		task->wakeup_tick = system_ticks + timeout_ticks;
	task->state = TASK_BLOCKED;
	task_list_push_back(&blocked_list, task, TASK_LIST_BLOCKED);
	scheduler_unlock_irqrestore(irq_flags);
	asm volatile("int %0" : : "i"(SCHEDULER_YIELD_VECTOR));

	return task->wait_result;
}

uint32_t scheduler_wake_channel(const void* wait_channel, uint32_t max_count) {
	if (wait_channel == NULL)
		return 0;

	const uint32_t irq_flags = scheduler_lock_irqsave();
	const uint32_t limit = max_count == 0 ? UINT_MAX : max_count;
	uint32_t woke = 0;

	task_t* task = blocked_list.head;
	while (task != NULL && woke < limit) {
		task_t* next = task->list_next;
		if (task->wait_channel == wait_channel) {
			task_list_remove(&blocked_list, task);
			task->wait_channel = NULL;
			task->wait_result = true;
			task->wakeup_tick = 0;
			scheduler_enqueue_runnable(task, SCHEDULER_ENQUEUE_REASON_WAKEUP);
			scheduler_trace_counter_inc(&scheduler_trace_counters.wakeups);
			woke++;
		}
		task = next;
	}

	scheduler_unlock_irqrestore(irq_flags);
	return woke;
}

bool scheduler_join_task(uint32_t task_id, uint32_t timeout_ticks) {
	if (task_id == 0 || timeout_ticks == 0)
		return false;

	const uint32_t current_task_id = scheduler_current_task_id();
	if (current_task_id != 0 && task_id == current_task_id)
		return false;

	const bool wait_forever = timeout_ticks == SCHEDULER_WAIT_FOREVER;
	const uint32_t start_tick = scheduler_ticks();

	for (;;) {
		scheduler_reap_resources_t resources;
		scheduler_reap_resources_reset(&resources);

		uint32_t wait_ticks = SCHEDULER_WAIT_FOREVER;
		const uint32_t irq_flags = scheduler_lock_irqsave();
		task_t* task = scheduler_find_task_by_id(task_id);
		if (task == NULL) {
			scheduler_unlock_irqrestore(irq_flags);
			return false;
		}

		if (task->state == TASK_ZOMBIE) {
			if (scheduler_detach_zombie_task_locked(task, &resources)) {
				scheduler_unlock_irqrestore(irq_flags);
				scheduler_cleanup_reaped_resources(&resources);
				return true;
			}

			scheduler_unlock_irqrestore(irq_flags);
			if (!wait_forever && (scheduler_ticks() - start_tick) >= timeout_ticks)
				return false;
			scheduler_yield();
			continue;
		}

		if (!wait_forever) {
			const uint32_t elapsed = system_ticks - start_tick;
			if (elapsed >= timeout_ticks) {
				scheduler_unlock_irqrestore(irq_flags);
				return false;
			}
			wait_ticks = timeout_ticks - elapsed;
		}

		scheduler_unlock_irqrestore(irq_flags);

		const bool woke = scheduler_wait_channel(&scheduler_join_wait_channel_token, wait_ticks);
		if (!woke)
			return false;
	}
}

uint32_t scheduler_reap_zombies(uint32_t max_count) {
	const uint32_t limit = max_count == 0 ? UINT_MAX : max_count;
	uint32_t reaped = 0;

	while (reaped < limit) {
		scheduler_reap_resources_t resources;
		scheduler_reap_resources_reset(&resources);

		bool detached = false;
		const uint32_t irq_flags = scheduler_lock_irqsave();
		task_t* task = zombie_list.head;
		while (task != NULL) {
			task_t* next = task->list_next;
			if (scheduler_detach_zombie_task_locked(task, &resources)) {
				detached = true;
				break;
			}
			task = next;
		}
		scheduler_unlock_irqrestore(irq_flags);

		if (!detached)
			break;

		scheduler_cleanup_reaped_resources(&resources);
		reaped++;
	}

	return reaped;
}

uint32_t scheduler_ticks(void) {
	return system_ticks;
}

void scheduler_yield(void) {
	asm volatile("int %0" : : "i"(SCHEDULER_YIELD_VECTOR));
}

__attribute__((__noreturn__))
void scheduler_exit_current(void) {
	const uint32_t irq_flags = scheduler_lock_irqsave();
	task_t* task = scheduler_current_task();
	if (task == NULL)
		panic("scheduler exit without current task");

	task->state = TASK_ZOMBIE;
	task_list_push_back(&zombie_list, task, TASK_LIST_ZOMBIE);
	scheduler_trace_counter_inc(&scheduler_trace_counters.task_exits);
	scheduler_unlock_irqrestore(irq_flags);
	(void) scheduler_wake_channel(&scheduler_join_wait_channel_token, 0);
	scheduler_yield();
	panic("scheduler returned to a dead task");
}

uint32_t scheduler_current_task_id(void) {
	task_t* task = scheduler_current_task();
	if (task == NULL)
		return 0;
	return task->id;
}

const char* scheduler_current_task_name(void) {
	task_t* task = scheduler_current_task();
	if (task == NULL || task->name == NULL)
		return "none";
	return task->name;
}

uint32_t scheduler_task_count(void) {
	const uint32_t irq_flags = scheduler_lock_irqsave();
	uint32_t count = 0;
	for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
		if (tasks[i].state != TASK_UNUSED)
			count++;
	}
	scheduler_unlock_irqrestore(irq_flags);
	return count;
}

bool scheduler_get_task_snapshot(uint32_t index, scheduler_task_snapshot_t* snapshot) {
	if (snapshot == NULL)
		return false;

	const uint32_t irq_flags = scheduler_lock_irqsave();
	uint32_t seen = 0;
	for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
		task_t* task = &tasks[i];
		if (task->state == TASK_UNUSED)
			continue;

		if (seen == index) {
			snapshot->id = task->id;
			snapshot->name = task->name != NULL ? task->name : "unnamed";
			snapshot->state = task_state_name(task->state);
			snapshot->wakeup_tick = task->wakeup_tick;
			snapshot->address_space_root =
				task->space != NULL ? paging_space_root_physical(task->space) : 0;
			snapshot->priority = (uint8_t) scheduler_priority_sanitize(task->priority);
			snapshot->cpu_id = task->assigned_cpu;
			snapshot->numa_node = task->assigned_numa_node;
			snapshot->cpu_affinity_mask = task->cpu_affinity_mask;
			scheduler_unlock_irqrestore(irq_flags);
			return true;
		}

		seen++;
	}

	scheduler_unlock_irqrestore(irq_flags);
	return false;
}

uint32_t scheduler_cpu_count(void) {
	return configured_cpu_count;
}

bool scheduler_get_cpu_snapshot(uint32_t index, scheduler_cpu_snapshot_t* snapshot) {
	if (snapshot == NULL)
		return false;

	const uint32_t irq_flags = scheduler_lock_irqsave();
	if (index >= configured_cpu_count) {
		scheduler_unlock_irqrestore(irq_flags);
		return false;
	}

	const scheduler_cpu_state_t* cpu = &cpus[index];
	snapshot->cpu_id = cpu->id;
	snapshot->numa_node = cpu->numa_node;
	snapshot->online = cpu->online;
	snapshot->runnable_count = cpu->runnable_count;
	snapshot->running_task_id = 0;
	snapshot->running_task_name = "none";

	if (cpu->current != NULL) {
		snapshot->running_task_id = cpu->current->id;
		snapshot->running_task_name =
			cpu->current->name != NULL ? cpu->current->name : "unnamed";
	}

	scheduler_unlock_irqrestore(irq_flags);
	return true;
}

void scheduler_get_list_snapshot(scheduler_list_snapshot_t* snapshot) {
	if (snapshot == NULL)
		return;

	const uint32_t irq_flags = scheduler_lock_irqsave();
	uint32_t runnable = 0;
	for (uint32_t cpu = 0; cpu < configured_cpu_count; cpu++)
		runnable += cpus[cpu].runnable_count;

	snapshot->free_count = free_list.count;
	snapshot->runnable_count = runnable;
	snapshot->sleeping_count = sleeping_list.count;
	snapshot->blocked_count = blocked_list.count;
	snapshot->zombie_count = zombie_list.count;
	scheduler_unlock_irqrestore(irq_flags);
}

void scheduler_trace_reset(void) {
	const uint32_t irq_flags = scheduler_lock_irqsave();
	scheduler_trace_reset_locked();
	scheduler_unlock_irqrestore(irq_flags);
}

bool scheduler_get_trace_counters(scheduler_trace_counters_t* counters) {
	if (counters == NULL)
		return false;

	const uint32_t irq_flags = scheduler_lock_irqsave();
	*counters = scheduler_trace_counters;
	scheduler_unlock_irqrestore(irq_flags);
	return true;
}

bool scheduler_get_trace_latency(scheduler_trace_latency_t* latency) {
	if (latency == NULL)
		return false;

	const uint32_t irq_flags = scheduler_lock_irqsave();
	*latency = scheduler_trace_latency;
	scheduler_unlock_irqrestore(irq_flags);
	return true;
}

static void scheduler_self_test_record(scheduler_self_test_report_t* report, bool condition) {
	if (report == NULL)
		return;

	report->checks++;
	if (!condition)
		report->failures++;
}

static bool scheduler_self_test_thread_list(scheduler_self_test_report_t* report) {
	task_list_t list;
	task_list_init(&list);

	task_t first;
	task_t second;
	task_t third;
	task_reset(&first);
	task_reset(&second);
	task_reset(&third);

	task_list_push_back(&list, &first, TASK_LIST_RUNNABLE);
	task_list_push_back(&list, &second, TASK_LIST_RUNNABLE);
	task_list_push_back(&list, &third, TASK_LIST_RUNNABLE);

	bool pass = true;
	scheduler_self_test_record(report, list.count == 3);
	pass = pass && list.count == 3;

	task_t* a = task_list_pop_front(&list);
	task_t* b = task_list_pop_front(&list);
	task_t* c = task_list_pop_front(&list);
	task_t* d = task_list_pop_front(&list);

	scheduler_self_test_record(report, a == &first);
	scheduler_self_test_record(report, b == &second);
	scheduler_self_test_record(report, c == &third);
	scheduler_self_test_record(report, d == NULL);
	scheduler_self_test_record(report, list.count == 0);

	pass = pass && a == &first && b == &second && c == &third && d == NULL && list.count == 0;
	return pass;
}

static bool scheduler_self_test_priority_dispatch(scheduler_self_test_report_t* report) {
	scheduler_cpu_state_t cpu;
	scheduler_cpu_state_init(&cpu, 0, 0, true);

	task_t low;
	task_t normal;
	task_t high;
	task_reset(&low);
	task_reset(&normal);
	task_reset(&high);

	low.priority = SCHEDULER_PRIORITY_LOW;
	normal.priority = SCHEDULER_PRIORITY_NORMAL;
	high.priority = SCHEDULER_PRIORITY_REALTIME;

	scheduler_cpu_enqueue_runnable_local(&cpu, &low);
	scheduler_cpu_enqueue_runnable_local(&cpu, &normal);
	scheduler_cpu_enqueue_runnable_local(&cpu, &high);

	task_t* first = scheduler_cpu_dequeue_next_runnable(&cpu);
	task_t* second = scheduler_cpu_dequeue_next_runnable(&cpu);
	task_t* third = scheduler_cpu_dequeue_next_runnable(&cpu);
	task_t* none = scheduler_cpu_dequeue_next_runnable(&cpu);

	bool pass = true;
	scheduler_self_test_record(report, first == &high);
	scheduler_self_test_record(report, second == &normal);
	scheduler_self_test_record(report, third == &low);
	scheduler_self_test_record(report, none == NULL);
	scheduler_self_test_record(report, cpu.runnable_count == 0);

	pass = pass && first == &high && second == &normal && third == &low;
	pass = pass && none == NULL && cpu.runnable_count == 0;
	return pass;
}

static bool scheduler_self_test_numa_selection(scheduler_self_test_report_t* report) {
	bool cpu_online[4] = { true, true, true, true };
	uint8_t cpu_to_node[4] = { 0, 0, 1, 1 };
	uint32_t cpu_load[4] = { 4, 2, 1, 3 };

	const uint8_t preferred_pick = scheduler_select_best_cpu_from_view(
		4, cpu_online, cpu_to_node, cpu_load,
		SCHEDULER_CPU_MASK_ALL, 1, 0);

	const uint8_t affinity_pick = scheduler_select_best_cpu_from_view(
		4, cpu_online, cpu_to_node, cpu_load,
		(1u << 0) | (1u << 3), SCHEDULER_NUMA_NODE_ANY, 0);

	cpu_online[3] = false;
	const uint8_t invalid_pick = scheduler_select_best_cpu_from_view(
		4, cpu_online, cpu_to_node, cpu_load,
		(1u << 3), SCHEDULER_NUMA_NODE_ANY, 0);

	bool pass = true;
	scheduler_self_test_record(report, preferred_pick == 2);
	scheduler_self_test_record(report, affinity_pick == 3);
	// When affinity resolves to no online CPUs, policy falls back to any online CPU.
	scheduler_self_test_record(report, invalid_pick == 2);

	pass = pass && preferred_pick == 2;
	pass = pass && affinity_pick == 3;
	pass = pass && invalid_pick == 2;
	return pass;
}

static bool scheduler_self_test_task_migration(scheduler_self_test_report_t* report) {
	scheduler_cpu_state_t cpu_a;
	scheduler_cpu_state_t cpu_b;
	scheduler_cpu_state_init(&cpu_a, 0, 0, true);
	scheduler_cpu_state_init(&cpu_b, 1, 0, true);

	task_t movable;
	task_t pinned;
	task_reset(&movable);
	task_reset(&pinned);

	movable.priority = SCHEDULER_PRIORITY_NORMAL;
	movable.cpu_affinity_mask = (1u << 0) | (1u << 1);
	pinned.priority = SCHEDULER_PRIORITY_NORMAL;
	pinned.cpu_affinity_mask = (1u << 0);

	scheduler_cpu_enqueue_runnable_local(&cpu_a, &movable);
	scheduler_cpu_enqueue_runnable_local(&cpu_a, &pinned);

	const bool moved = scheduler_migrate_runnable_task(&cpu_a, &cpu_b, &movable, false);
	const bool blocked = scheduler_migrate_runnable_task(&cpu_a, &cpu_b, &pinned, false);

	bool pass = true;
	scheduler_self_test_record(report, moved);
	scheduler_self_test_record(report, !blocked);
	scheduler_self_test_record(report, movable.assigned_cpu == 1);
	scheduler_self_test_record(report, pinned.assigned_cpu == 0);
	scheduler_self_test_record(report, cpu_a.runnable_count == 1);
	scheduler_self_test_record(report, cpu_b.runnable_count == 1);

	pass = pass && moved;
	pass = pass && !blocked;
	pass = pass && movable.assigned_cpu == 1;
	pass = pass && pinned.assigned_cpu == 0;
	pass = pass && cpu_a.runnable_count == 1;
	pass = pass && cpu_b.runnable_count == 1;
	return pass;
}

static bool scheduler_self_test_load_balancer(scheduler_self_test_report_t* report) {
	scheduler_cpu_state_t cpu_view[3];
	for (uint32_t i = 0; i < 3; i++)
		scheduler_cpu_state_init(&cpu_view[i], (uint8_t) i, 0, true);

	task_t pinned;
	task_t flexible_a;
	task_t flexible_b;
	task_reset(&pinned);
	task_reset(&flexible_a);
	task_reset(&flexible_b);

	pinned.priority = SCHEDULER_PRIORITY_LOW;
	pinned.cpu_affinity_mask = (1u << 0);
	flexible_a.priority = SCHEDULER_PRIORITY_BACKGROUND;
	flexible_a.cpu_affinity_mask = (1u << 0) | (1u << 1) | (1u << 2);
	flexible_b.priority = SCHEDULER_PRIORITY_BACKGROUND;
	flexible_b.cpu_affinity_mask = (1u << 0) | (1u << 1) | (1u << 2);

	scheduler_cpu_enqueue_runnable_local(&cpu_view[0], &pinned);
	scheduler_cpu_enqueue_runnable_local(&cpu_view[0], &flexible_a);
	scheduler_cpu_enqueue_runnable_local(&cpu_view[0], &flexible_b);

	bool cpu_online[3] = { true, true, true };
	uint8_t cpu_to_node[3] = { 0, 0, 0 };
	uint32_t cpu_load[3] = {
		scheduler_cpu_load(&cpu_view[0]),
		scheduler_cpu_load(&cpu_view[1]),
		scheduler_cpu_load(&cpu_view[2])
	};

	const bool first_move = scheduler_balance_once_from_view(
		cpu_view, 3, cpu_online, cpu_to_node, cpu_load, false);
	const bool second_move = scheduler_balance_once_from_view(
		cpu_view, 3, cpu_online, cpu_to_node, cpu_load, false);
	const bool third_move = scheduler_balance_once_from_view(
		cpu_view, 3, cpu_online, cpu_to_node, cpu_load, false);

	bool pass = true;
	scheduler_self_test_record(report, first_move);
	scheduler_self_test_record(report, second_move);
	scheduler_self_test_record(report, !third_move);
	scheduler_self_test_record(report, pinned.assigned_cpu == 0);
	scheduler_self_test_record(report, cpu_view[0].runnable_count == 1);
	scheduler_self_test_record(report, cpu_view[1].runnable_count == 1);
	scheduler_self_test_record(report, cpu_view[2].runnable_count == 1);

	pass = pass && first_move;
	pass = pass && second_move;
	pass = pass && !third_move;
	pass = pass && pinned.assigned_cpu == 0;
	pass = pass && cpu_view[0].runnable_count == 1;
	pass = pass && cpu_view[1].runnable_count == 1;
	pass = pass && cpu_view[2].runnable_count == 1;
	return pass;
}

static bool scheduler_self_test_trace_latency_metrics(scheduler_self_test_report_t* report) {
	scheduler_trace_latency_metric_t metric;
	scheduler_trace_latency_reset_metric(&metric);
	scheduler_trace_latency_record(&metric, 7u);
	scheduler_trace_latency_record(&metric, 3u);
	scheduler_trace_latency_record(&metric, 11u);

	bool pass = true;
	scheduler_self_test_record(report, metric.sample_count == 3u);
	scheduler_self_test_record(report, metric.total_ticks == 21u);
	scheduler_self_test_record(report, metric.min_ticks == 3u);
	scheduler_self_test_record(report, metric.max_ticks == 11u);

	pass = pass && metric.sample_count == 3u;
	pass = pass && metric.total_ticks == 21u;
	pass = pass && metric.min_ticks == 3u;
	pass = pass && metric.max_ticks == 11u;
	return pass;
}

bool scheduler_run_self_tests(scheduler_self_test_report_t* report) {
	scheduler_self_test_report_t local_report = { 0, 0 };
	if (report == NULL)
		report = &local_report;
	else {
		report->checks = 0;
		report->failures = 0;
	}

	bool pass = true;
	pass = scheduler_self_test_thread_list(report) && pass;
	pass = scheduler_self_test_priority_dispatch(report) && pass;
	pass = scheduler_self_test_numa_selection(report) && pass;
	pass = scheduler_self_test_task_migration(report) && pass;
	pass = scheduler_self_test_load_balancer(report) && pass;
	pass = scheduler_self_test_trace_latency_metrics(report) && pass;
	return pass && report->failures == 0;
}
