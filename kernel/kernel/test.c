#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/test.h>

#define KTEST_MAX_SUBSYSTEMS 32u

typedef struct ktest_entry {
	const char* name;
	ktest_hook_t hook;
} ktest_entry_t;

static ktest_entry_t ktest_registry[KTEST_MAX_SUBSYSTEMS];
static uint32_t ktest_registry_count;

static bool ktest_selector_matches(const char* name, const char* selector) {
	return selector == NULL || selector[0] == '\0' ||
		strcmp(selector, "all") == 0 || strcmp(name, selector) == 0;
}

static void ktest_normalize_result(ktest_stats_t* stats, bool* pass) {
	if (stats == NULL || pass == NULL)
		return;

	if (stats->checks == 0) {
		stats->checks = 1;
		if (!*pass)
			stats->failures = 1;
	}

	if (stats->failures > stats->checks)
		stats->failures = stats->checks;

	*pass = stats->failures == 0;
}

bool ktest_register_subsystem(const char* name, ktest_hook_t hook) {
	if (name == NULL || name[0] == '\0' || hook == NULL)
		return false;

	for (uint32_t i = 0; i < ktest_registry_count; i++) {
		if (strcmp(ktest_registry[i].name, name) == 0) {
			ktest_registry[i].hook = hook;
			return true;
		}
	}

	if (ktest_registry_count >= KTEST_MAX_SUBSYSTEMS)
		return false;

	ktest_registry[ktest_registry_count].name = name;
	ktest_registry[ktest_registry_count].hook = hook;
	ktest_registry_count++;
	return true;
}

uint32_t ktest_registered_count(void) {
	return ktest_registry_count;
}

const char* ktest_registered_name(uint32_t index) {
	if (index >= ktest_registry_count)
		return NULL;
	return ktest_registry[index].name;
}

void ktest_stats_reset(ktest_stats_t* stats) {
	if (stats == NULL)
		return;
	stats->checks = 0;
	stats->failures = 0;
}

void ktest_stats_record(ktest_stats_t* stats, bool condition) {
	if (stats == NULL)
		return;

	stats->checks++;
	if (!condition)
		stats->failures++;
}

bool ktest_stats_passed(const ktest_stats_t* stats) {
	if (stats == NULL)
		return false;
	return stats->failures == 0;
}

bool ktest_run(const char* selector, ktest_run_summary_t* summary,
	ktest_reporter_t reporter, void* reporter_context) {
	ktest_run_summary_t local_summary = { 0, 0, 0, 0, 0 };

	for (uint32_t i = 0; i < ktest_registry_count; i++) {
		const ktest_entry_t* entry = &ktest_registry[i];
		if (!ktest_selector_matches(entry->name, selector))
			continue;

		ktest_stats_t stats;
		ktest_stats_reset(&stats);

		bool pass = entry->hook(&stats);
		ktest_normalize_result(&stats, &pass);

		local_summary.executed++;
		local_summary.checks += stats.checks;
		local_summary.check_failures += stats.failures;
		if (pass)
			local_summary.passed++;
		else
			local_summary.failed++;

		if (reporter != NULL)
			reporter(entry->name, &stats, pass, reporter_context);
	}

	if (summary != NULL)
		*summary = local_summary;

	return local_summary.executed != 0;
}
