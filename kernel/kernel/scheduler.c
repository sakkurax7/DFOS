#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <kernel/heap.h>
#include <kernel/interrupts.h>
#include <kernel/panic.h>
#include <kernel/scheduler.h>

#define MAX_TASKS         16
#define TASK_STACK_SIZE   16384
#define SCHEDULER_YIELD_VECTOR 48u
#define TIME_SLICE_TICKS  5

typedef enum task_state {
	TASK_UNUSED = 0,
	TASK_RUNNABLE,
	TASK_RUNNING,
	TASK_SLEEPING,
	TASK_ZOMBIE
} task_state_t;

typedef struct task {
	uint32_t id;
	const char* name;
	task_state_t state;
	interrupt_frame_t* frame;
	void* stack_base;
	uint32_t wakeup_tick;
	kernel_task_entry_t entry;
	void* arg;
} task_t;

static task_t tasks[MAX_TASKS];
static uint32_t current_task_index;
static uint32_t ticks_since_switch;
static uint32_t system_ticks;
static uint32_t next_task_id;

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
	case TASK_ZOMBIE:
		return "zombie";
	}

	return "unknown";
}

static task_t* current_task(void) {
	return &tasks[current_task_index];
}

static void task_trampoline(void) {
	task_t* task = current_task();
	// Newly created tasks start here after the first context switch into their prepared frame.
	task->entry(task->arg);
	scheduler_exit_current();
}

static interrupt_frame_t* schedule_next(interrupt_frame_t* frame, bool force_switch) {
	task_t* current = current_task();
	// The scheduler reuses the interrupt frame as the thread's saved CPU context.
	current->frame = frame;
	if (current->state == TASK_RUNNING)
		current->state = TASK_RUNNABLE;

	for (size_t offset = 1; offset <= MAX_TASKS; offset++) {
		const uint32_t index = (current_task_index + offset) % MAX_TASKS;
		task_t* candidate = &tasks[index];

		if (candidate->state != TASK_RUNNABLE)
			continue;

		if (!force_switch && index == current_task_index)
			break;

		current_task_index = index;
		candidate->state = TASK_RUNNING;
		ticks_since_switch = 0;
		return candidate->frame;
	}

	current->state = TASK_RUNNING;
	return frame;
}

static task_t* allocate_task_slot(void) {
	for (size_t i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state == TASK_UNUSED)
			return &tasks[i];
	}
	return NULL;
}

void scheduler_init(void) {
	for (size_t i = 0; i < MAX_TASKS; i++)
		tasks[i].state = TASK_UNUSED;
	current_task_index = 0;
	ticks_since_switch = 0;
	system_ticks = 0;
	next_task_id = 1;
	register_interrupt_handler(SCHEDULER_YIELD_VECTOR, scheduler_on_yield);
}

void scheduler_bootstrap_current(const char* name) {
	tasks[0].id = next_task_id++;
	tasks[0].name = name;
	tasks[0].state = TASK_RUNNING;
	tasks[0].frame = NULL;
	tasks[0].stack_base = NULL;
	tasks[0].wakeup_tick = 0;
	tasks[0].entry = NULL;
	tasks[0].arg = NULL;
	current_task_index = 0;
}

bool scheduler_create_kernel_task(const char* name, kernel_task_entry_t entry, void* arg) {
	task_t* task = allocate_task_slot();
	if (task == NULL)
		return false;

	uint8_t* stack = (uint8_t*) kmalloc(TASK_STACK_SIZE);
	interrupt_frame_t* frame =
		(interrupt_frame_t*) (stack + TASK_STACK_SIZE - sizeof(interrupt_frame_t));

	// This synthetic frame matches what the ISR epilogue expects to pop with `iret`.
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
	frame->cs = 0x08;
	frame->eflags = 0x202;

	task->id = next_task_id++;
	task->name = name;
	task->state = TASK_RUNNABLE;
	task->frame = frame;
	task->stack_base = stack;
	task->wakeup_tick = 0;
	task->entry = entry;
	task->arg = arg;
	return true;
}

interrupt_frame_t* scheduler_on_timer_tick(interrupt_frame_t* frame) {
	system_ticks++;
	ticks_since_switch++;

	for (size_t i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state == TASK_SLEEPING && tasks[i].wakeup_tick <= system_ticks)
			tasks[i].state = TASK_RUNNABLE;
	}

	if (ticks_since_switch >= TIME_SLICE_TICKS)
		return schedule_next(frame, true);

	current_task()->frame = frame;
	return frame;
}

interrupt_frame_t* scheduler_on_yield(interrupt_frame_t* frame) {
	return schedule_next(frame, true);
}

void scheduler_sleep(uint32_t ticks) {
	task_t* task = current_task();
	task->wakeup_tick = system_ticks + ticks;
	task->state = TASK_SLEEPING;
	// Reuse the scheduler's software interrupt path so sleep and explicit yield share one exit.
	asm volatile("int %0" : : "i"(SCHEDULER_YIELD_VECTOR));
}

uint32_t scheduler_ticks(void) {
	return system_ticks;
}

void scheduler_yield(void) {
	asm volatile("int %0" : : "i"(SCHEDULER_YIELD_VECTOR));
}

__attribute__((__noreturn__))
void scheduler_exit_current(void) {
	// Dead tasks stay in the table for inspection until the scheduler is taught to reap them.
	current_task()->state = TASK_ZOMBIE;
	scheduler_yield();
	panic("scheduler returned to a dead task");
}

uint32_t scheduler_current_task_id(void) {
	return current_task()->id;
}

const char* scheduler_current_task_name(void) {
	return current_task()->name;
}

uint32_t scheduler_task_count(void) {
	uint32_t count = 0;
	for (size_t i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state != TASK_UNUSED)
			count++;
	}
	return count;
}

bool scheduler_get_task_snapshot(uint32_t index, scheduler_task_snapshot_t* snapshot) {
	uint32_t seen = 0;

	for (size_t i = 0; i < MAX_TASKS; i++) {
		if (tasks[i].state == TASK_UNUSED)
			continue;

		if (seen == index) {
			snapshot->id = tasks[i].id;
			snapshot->name = tasks[i].name;
			snapshot->state = task_state_name(tasks[i].state);
			snapshot->wakeup_tick = tasks[i].wakeup_tick;
			return true;
		}

		seen++;
	}

	return false;
}
