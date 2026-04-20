#ifndef KERNEL_TEST_H
#define KERNEL_TEST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct ktest_stats {
	uint32_t checks;
	uint32_t failures;
} ktest_stats_t;

typedef struct ktest_run_summary {
	uint32_t executed;
	uint32_t passed;
	uint32_t failed;
	uint32_t checks;
	uint32_t check_failures;
} ktest_run_summary_t;

typedef bool (*ktest_hook_t)(ktest_stats_t* stats);
typedef void (*ktest_reporter_t)(const char* name, const ktest_stats_t* stats,
	bool pass, void* context);

bool ktest_register_subsystem(const char* name, ktest_hook_t hook);
uint32_t ktest_registered_count(void);
const char* ktest_registered_name(uint32_t index);

void ktest_stats_reset(ktest_stats_t* stats);
void ktest_stats_record(ktest_stats_t* stats, bool condition);
bool ktest_stats_passed(const ktest_stats_t* stats);

bool ktest_run(const char* selector, ktest_run_summary_t* summary,
	ktest_reporter_t reporter, void* reporter_context);

#endif
