#ifndef KERNEL_SCHEDULER_H
#define KERNEL_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include <kernel/interrupts.h>

typedef void (*kernel_task_entry_t)(void* arg);

typedef struct scheduler_task_snapshot {
	uint32_t id;
	const char* name;
	const char* state;
	uint32_t wakeup_tick;
} scheduler_task_snapshot_t;

void scheduler_init(void);
void scheduler_bootstrap_current(const char* name);
bool scheduler_create_kernel_task(const char* name, kernel_task_entry_t entry, void* arg);
interrupt_frame_t* scheduler_on_timer_tick(interrupt_frame_t* frame);
interrupt_frame_t* scheduler_on_yield(interrupt_frame_t* frame);
void scheduler_sleep(uint32_t ticks);
void scheduler_yield(void);
__attribute__((__noreturn__)) void scheduler_exit_current(void);
uint32_t scheduler_ticks(void);
uint32_t scheduler_current_task_id(void);
const char* scheduler_current_task_name(void);
uint32_t scheduler_task_count(void);
bool scheduler_get_task_snapshot(uint32_t index, scheduler_task_snapshot_t* snapshot);

#endif
