#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/cpu.h>
#include <kernel/gdt.h>
#include <kernel/heap.h>
#include <kernel/interrupts.h>
#include <kernel/paging.h>
#include <kernel/panic.h>
#include <kernel/scheduler.h>
#include <kernel/x86.h>

#define SCHEDULER_YIELD_VECTOR   48u
#define TASK_STACK_SIZE          16384u
#define SCHEDULER_INVALID_CPU_ID 0xFFu

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
static bool scheduler_bootstrapped;

static const uint8_t priority_time_slice[SCHEDULER_PRIORITY_COUNT] = {
	2u, 3u, 5u, 7u, 9u
};

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

	if (scheduler_bootstrapped || scheduler_has_live_tasks())
		return false;

	if (config->cpu_count == 0 || config->cpu_count > SCHEDULER_MAX_CPUS)
		return false;

	if (config->node_count == 0 || config->node_count > SCHEDULER_MAX_NUMA_NODES)
		return false;

	if (config->boot_cpu_id >= config->cpu_count)
		return false;

	uint32_t online_count = 0;
	for (uint32_t i = 0; i < config->cpu_count; i++) {
		if (config->cpu_to_node[i] >= config->node_count)
			return false;
		if (config->cpu_online[i])
			online_count++;
	}

	if (online_count == 0 || !config->cpu_online[config->boot_cpu_id])
		return false;

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

	return true;
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

static void scheduler_cpu_enqueue_runnable(scheduler_cpu_state_t* cpu, task_t* task) {
	const scheduler_priority_t priority = scheduler_priority_sanitize(task->priority);
	task->priority = priority;
	task->assigned_cpu = cpu->id;
	task->assigned_numa_node = cpu->numa_node;
	task->state = TASK_RUNNABLE;
	task->ticks_in_slice = 0;
	task_list_push_back(&cpu->runnable[(uint32_t) priority], task, TASK_LIST_RUNNABLE);
	cpu->runnable_count++;
}

static void scheduler_enqueue_runnable(task_t* task) {
	scheduler_cpu_state_t* cpu = scheduler_cpu_by_id(task->assigned_cpu);
	scheduler_cpu_enqueue_runnable(cpu, task);
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
			return next;
		}
	}

	return NULL;
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
			task_list_remove(&sleeping_list, task);
			scheduler_enqueue_runnable(task);
		}
		task = next;
	}

	task = blocked_list.head;
	while (task != NULL) {
		task_t* next = task->list_next;
		if (task->wakeup_tick != 0 && task->wakeup_tick <= system_ticks) {
			task_list_remove(&blocked_list, task);
			task->wait_channel = NULL;
			task->wait_result = false;
			task->wakeup_tick = 0;
			scheduler_enqueue_runnable(task);
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

	next->state = TASK_RUNNING;
	next->ticks_in_slice = 0;
	cpu->current = next;
	gdt_set_kernel_stack(next->kernel_stack_top);
	paging_switch_space(next->space);
	return next->frame;
}

static interrupt_frame_t* scheduler_schedule_cpu(
	scheduler_cpu_state_t* cpu, interrupt_frame_t* frame, bool force_switch) {
	task_t* current = cpu->current;

	if (current != NULL)
		current->frame = frame;

	if (current != NULL && current->state == TASK_RUNNING && !force_switch) {
		if (!scheduler_cpu_has_higher_priority_runnable(cpu, current->priority))
			return frame;
		force_switch = true;
	}

	if (current != NULL && current->state == TASK_RUNNING && force_switch)
		scheduler_cpu_enqueue_runnable(cpu, current);

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
	configured_cpu_count = 0;
	configured_node_count = 0;
	boot_cpu_id = 0;

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

void scheduler_bootstrap_current(const char* name) {
	if (scheduler_bootstrapped)
		panic("scheduler bootstrap attempted twice");
	if (configured_cpu_count == 0)
		panic("scheduler topology is not configured");

	task_t* task = scheduler_allocate_task_slot();
	if (task == NULL)
		panic("scheduler could not allocate bootstrap task");

	uint32_t bootstrap_stack_top;
	asm volatile("mov %%esp, %0" : "=r"(bootstrap_stack_top));

	const char* task_name = name != NULL ? name : "bootstrap";
	scheduler_cpu_state_t* cpu = scheduler_cpu_by_id(boot_cpu_id);

	task->id = next_task_id++;
	task->name = task_name;
	task->state = TASK_RUNNING;
	task->priority = SCHEDULER_PRIORITY_NORMAL;
	task->cpu_affinity_mask = scheduler_online_cpu_mask();
	task->preferred_numa_node = SCHEDULER_NUMA_NODE_ANY;
	task->assigned_cpu = cpu->id;
	task->assigned_numa_node = cpu->numa_node;
	task->frame = NULL;
	task->stack_base = NULL;
	task->kernel_stack_top = bootstrap_stack_top;
	task->wakeup_tick = 0;
	task->entry = NULL;
	task->arg = NULL;
	task->space = paging_current_space();
	task->ticks_in_slice = 0;

	cpu->current = task;
	scheduler_bootstrapped = true;
	gdt_set_kernel_stack(task->kernel_stack_top);
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
	if (entry == NULL || configured_cpu_count == 0)
		return false;

	scheduler_task_config_t effective;
	scheduler_task_config_default(&effective);
	if (config != NULL)
		effective = *config;

	effective.priority = scheduler_priority_sanitize(effective.priority);
	effective.cpu_affinity_mask = scheduler_normalize_affinity_mask(effective.cpu_affinity_mask);
	if (effective.preferred_numa_node != SCHEDULER_NUMA_NODE_ANY &&
		effective.preferred_numa_node >= configured_node_count)
		effective.preferred_numa_node = SCHEDULER_NUMA_NODE_ANY;

	task_t* task = scheduler_allocate_task_slot();
	if (task == NULL)
		return false;

	paging_space_t* space = paging_create_process_space();
	if (space == NULL) {
		scheduler_release_task_slot(task);
		return false;
	}

	uint8_t* stack = (uint8_t*) kmalloc(TASK_STACK_SIZE);
	if (stack == NULL) {
		paging_destroy_process_space(space);
		scheduler_release_task_slot(task);
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
	task->entry = entry;
	task->arg = arg;
	task->space = space;
	task->ticks_in_slice = 0;

	scheduler_cpu_enqueue_runnable(cpu, task);
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
	task_t* task = scheduler_find_task_by_id(task_id);
	if (task == NULL)
		return false;

	const scheduler_priority_t safe_priority = scheduler_priority_sanitize(priority);
	if (task->priority == safe_priority)
		return true;

	if (task->state == TASK_RUNNABLE && task->list_kind == TASK_LIST_RUNNABLE)
		scheduler_remove_runnable(task);

	task->priority = safe_priority;

	if (task->state == TASK_RUNNABLE)
		scheduler_enqueue_runnable(task);

	if (task->state == TASK_RUNNING)
		task->ticks_in_slice = 0;

	return true;
}

interrupt_frame_t* scheduler_on_timer_tick(interrupt_frame_t* frame) {
	system_ticks++;
	scheduler_advance_timers();

	scheduler_cpu_state_t* cpu = scheduler_current_cpu_state();
	task_t* current = cpu->current;

	if (current == NULL)
		return scheduler_schedule_cpu(cpu, frame, true);

	if (current->state != TASK_RUNNING)
		return scheduler_schedule_cpu(cpu, frame, true);

	current->ticks_in_slice++;
	const uint8_t slice = scheduler_priority_timeslice(current->priority);
	if (current->ticks_in_slice >= slice ||
		scheduler_cpu_has_higher_priority_runnable(cpu, current->priority))
		return scheduler_schedule_cpu(cpu, frame, true);

	current->frame = frame;
	return frame;
}

interrupt_frame_t* scheduler_on_yield(interrupt_frame_t* frame) {
	scheduler_cpu_state_t* cpu = scheduler_current_cpu_state();
	return scheduler_schedule_cpu(cpu, frame, true);
}

void scheduler_sleep(uint32_t ticks) {
	if (ticks == 0) {
		scheduler_yield();
		return;
	}

	task_t* task = scheduler_current_task();
	if (task == NULL)
		return;

	task->wakeup_tick = system_ticks + ticks;
	task->state = TASK_SLEEPING;
	task_list_push_back(&sleeping_list, task, TASK_LIST_SLEEPING);
	asm volatile("int %0" : : "i"(SCHEDULER_YIELD_VECTOR));
}

bool scheduler_wait_channel(const void* wait_channel, uint32_t timeout_ticks) {
	if (wait_channel == NULL || timeout_ticks == 0)
		return false;

	task_t* task = scheduler_current_task();
	if (task == NULL)
		return false;

	const uint32_t irq_flags = scheduler_irq_save();
	task->wait_channel = wait_channel;
	task->wait_result = false;
	if (timeout_ticks == SCHEDULER_WAIT_FOREVER)
		task->wakeup_tick = 0;
	else
		task->wakeup_tick = system_ticks + timeout_ticks;
	task->state = TASK_BLOCKED;
	task_list_push_back(&blocked_list, task, TASK_LIST_BLOCKED);
	asm volatile("int %0" : : "i"(SCHEDULER_YIELD_VECTOR));
	scheduler_irq_restore(irq_flags);

	return task->wait_result;
}

uint32_t scheduler_wake_channel(const void* wait_channel, uint32_t max_count) {
	if (wait_channel == NULL)
		return 0;

	const uint32_t irq_flags = scheduler_irq_save();
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
			scheduler_enqueue_runnable(task);
			woke++;
		}
		task = next;
	}

	scheduler_irq_restore(irq_flags);
	return woke;
}

uint32_t scheduler_ticks(void) {
	return system_ticks;
}

void scheduler_yield(void) {
	asm volatile("int %0" : : "i"(SCHEDULER_YIELD_VECTOR));
}

__attribute__((__noreturn__))
void scheduler_exit_current(void) {
	task_t* task = scheduler_current_task();
	if (task == NULL)
		panic("scheduler exit without current task");

	task->state = TASK_ZOMBIE;
	task_list_push_back(&zombie_list, task, TASK_LIST_ZOMBIE);
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
	uint32_t count = 0;
	for (uint32_t i = 0; i < SCHEDULER_MAX_THREADS; i++) {
		if (tasks[i].state != TASK_UNUSED)
			count++;
	}
	return count;
}

bool scheduler_get_task_snapshot(uint32_t index, scheduler_task_snapshot_t* snapshot) {
	if (snapshot == NULL)
		return false;

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
			return true;
		}

		seen++;
	}

	return false;
}

uint32_t scheduler_cpu_count(void) {
	return configured_cpu_count;
}

bool scheduler_get_cpu_snapshot(uint32_t index, scheduler_cpu_snapshot_t* snapshot) {
	if (snapshot == NULL || index >= configured_cpu_count)
		return false;

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

	return true;
}

void scheduler_get_list_snapshot(scheduler_list_snapshot_t* snapshot) {
	if (snapshot == NULL)
		return;

	uint32_t runnable = 0;
	for (uint32_t cpu = 0; cpu < configured_cpu_count; cpu++)
		runnable += cpus[cpu].runnable_count;

	snapshot->free_count = free_list.count;
	snapshot->runnable_count = runnable;
	snapshot->sleeping_count = sleeping_list.count;
	snapshot->blocked_count = blocked_list.count;
	snapshot->zombie_count = zombie_list.count;
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

	scheduler_cpu_enqueue_runnable(&cpu, &low);
	scheduler_cpu_enqueue_runnable(&cpu, &normal);
	scheduler_cpu_enqueue_runnable(&cpu, &high);

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
	return pass && report->failures == 0;
}
