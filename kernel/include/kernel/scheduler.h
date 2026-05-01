#ifndef KERNEL_SCHEDULER_H
#define KERNEL_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#include <kernel/interrupts.h>

#define SCHEDULER_MAX_THREADS     32u
#define SCHEDULER_MAX_CPUS        8u
#define SCHEDULER_MAX_NUMA_NODES  4u
#define SCHEDULER_CPU_MASK_ALL    0xFFFFFFFFu
#define SCHEDULER_NUMA_NODE_ANY   0xFFu
#define SCHEDULER_WAIT_FOREVER    0xFFFFFFFFu

typedef void (*kernel_task_entry_t)(void* arg);

typedef enum scheduler_priority {
	SCHEDULER_PRIORITY_REALTIME = 0,
	SCHEDULER_PRIORITY_HIGH,
	SCHEDULER_PRIORITY_NORMAL,
	SCHEDULER_PRIORITY_LOW,
	SCHEDULER_PRIORITY_BACKGROUND,
	SCHEDULER_PRIORITY_COUNT
} scheduler_priority_t;

typedef struct scheduler_task_config {
	scheduler_priority_t priority;
	uint32_t cpu_affinity_mask;
	uint8_t preferred_numa_node;
} scheduler_task_config_t;

typedef struct scheduler_topology_config {
	uint8_t cpu_count;
	uint8_t boot_cpu_id;
	uint8_t node_count;
	uint8_t cpu_to_node[SCHEDULER_MAX_CPUS];
	bool cpu_online[SCHEDULER_MAX_CPUS];
} scheduler_topology_config_t;

typedef struct scheduler_task_snapshot {
	uint32_t id;
	const char* name;
	const char* state;
	uint32_t wakeup_tick;
	uint32_t address_space_root;
	uint8_t priority;
	uint8_t cpu_id;
	uint8_t numa_node;
	uint32_t cpu_affinity_mask;
} scheduler_task_snapshot_t;

typedef struct scheduler_cpu_snapshot {
	uint8_t cpu_id;
	uint8_t numa_node;
	bool online;
	uint32_t runnable_count;
	uint32_t running_task_id;
	const char* running_task_name;
} scheduler_cpu_snapshot_t;

typedef struct scheduler_list_snapshot {
	uint32_t free_count;
	uint32_t runnable_count;
	uint32_t sleeping_count;
	uint32_t blocked_count;
	uint32_t zombie_count;
} scheduler_list_snapshot_t;

typedef struct scheduler_self_test_report {
	uint32_t checks;
	uint32_t failures;
} scheduler_self_test_report_t;

typedef struct scheduler_trace_counters {
	uint64_t timer_ticks;
	uint64_t schedule_events;
	uint64_t context_switches;
	uint64_t preemptions;
	uint64_t voluntary_yields;
	uint64_t wakeups;
	uint64_t wait_calls;
	uint64_t wait_timeouts;
	uint64_t sleep_calls;
	uint64_t task_creations;
	uint64_t task_exits;
	uint64_t priority_updates;
	uint64_t load_balance_runs;
	uint64_t load_balance_migrations;
	uint64_t task_migrations;
	uint64_t remote_wakeup_ipis;
	uint64_t remote_reschedule_ipis;
} scheduler_trace_counters_t;

typedef struct scheduler_trace_latency_metric {
	uint64_t sample_count;
	uint64_t total_ticks;
	uint32_t min_ticks;
	uint32_t max_ticks;
} scheduler_trace_latency_metric_t;

typedef struct scheduler_trace_latency {
	scheduler_trace_latency_metric_t runnable_wait_ticks;
	scheduler_trace_latency_metric_t sleep_overshoot_ticks;
	scheduler_trace_latency_metric_t wait_timeout_overshoot_ticks;
} scheduler_trace_latency_t;

void scheduler_init(void);
void scheduler_topology_default(scheduler_topology_config_t* config);
bool scheduler_configure_topology(const scheduler_topology_config_t* config);
void scheduler_task_config_default(scheduler_task_config_t* config);
void scheduler_bootstrap_current(const char* name);
bool scheduler_bootstrap_secondary_current(
	uint32_t cpu_id, uint32_t kernel_stack_top, const char* name);
bool scheduler_create_kernel_task(const char* name, kernel_task_entry_t entry, void* arg);
bool scheduler_create_kernel_task_ex(const char* name, kernel_task_entry_t entry,
	void* arg, const scheduler_task_config_t* config);
bool scheduler_set_task_priority(uint32_t task_id, scheduler_priority_t priority);
interrupt_frame_t* scheduler_on_timer_tick(interrupt_frame_t* frame);
interrupt_frame_t* scheduler_on_yield(interrupt_frame_t* frame);
void scheduler_sleep(uint32_t ticks);
bool scheduler_wait_channel(const void* wait_channel, uint32_t timeout_ticks);
uint32_t scheduler_wake_channel(const void* wait_channel, uint32_t max_count);
bool scheduler_join_task(uint32_t task_id, uint32_t timeout_ticks);
uint32_t scheduler_reap_zombies(uint32_t max_count);
void scheduler_yield(void);
__attribute__((__noreturn__)) void scheduler_exit_current(void);
uint32_t scheduler_ticks(void);
uint32_t scheduler_current_task_id(void);
const char* scheduler_current_task_name(void);
uint32_t scheduler_current_cpu(void);
uint32_t scheduler_task_count(void);
bool scheduler_get_task_snapshot(uint32_t index, scheduler_task_snapshot_t* snapshot);
uint32_t scheduler_cpu_count(void);
bool scheduler_get_cpu_snapshot(uint32_t index, scheduler_cpu_snapshot_t* snapshot);
void scheduler_get_list_snapshot(scheduler_list_snapshot_t* snapshot);
void scheduler_trace_reset(void);
bool scheduler_get_trace_counters(scheduler_trace_counters_t* counters);
bool scheduler_get_trace_latency(scheduler_trace_latency_t* latency);
bool scheduler_run_self_tests(scheduler_self_test_report_t* report);

#endif
