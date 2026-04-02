#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/module.h>

#define MODULE_REGISTRY_CAPACITY 32

static const module_descriptor_t* registered_modules[MODULE_REGISTRY_CAPACITY];
static bool active_modules[MODULE_REGISTRY_CAPACITY];
static bool attempted_modules[MODULE_REGISTRY_CAPACITY];
static size_t registered_module_count;

static bool module_is_supported(const module_descriptor_t* module) {
	if (module == NULL)
		return false;

	if (module->probe == NULL)
		return true;

	return module->probe();
}

static bool module_matches_kind(const module_descriptor_t* module, module_kind_t kind) {
	return module != NULL && module->kind == kind;
}

static bool module_find_best_candidate(module_kind_t kind, size_t* index_out) {
	size_t best_index = 0;
	uint32_t best_priority = 0;
	bool found = false;

	for (size_t i = 0; i < registered_module_count; i++) {
		const module_descriptor_t* module = registered_modules[i];
		if (active_modules[i] || attempted_modules[i] ||
				!module_matches_kind(module, kind) || !module_is_supported(module))
			continue;

		if (!found || module->priority > best_priority) {
			best_index = i;
			best_priority = module->priority;
			found = true;
		}
	}

	if (!found)
		return false;

	*index_out = best_index;
	return true;
}

void module_register(const module_descriptor_t* module) {
	if (module == NULL || registered_module_count >= MODULE_REGISTRY_CAPACITY)
		return;

	registered_modules[registered_module_count] = module;
	active_modules[registered_module_count] = false;
	attempted_modules[registered_module_count] = false;
	registered_module_count++;
}

bool module_activate_best(module_kind_t kind) {
	size_t index;

	while (module_find_best_candidate(kind, &index)) {
		const module_descriptor_t* module = registered_modules[index];
		attempted_modules[index] = true;
		if (module->activate == NULL || !module->activate())
			continue;

		active_modules[index] = true;
		return true;
	}

	return false;
}

size_t module_activate_all(module_kind_t kind) {
	size_t activated = 0;
	size_t index;

	while (module_find_best_candidate(kind, &index)) {
		const module_descriptor_t* module = registered_modules[index];
		attempted_modules[index] = true;
		if (module->activate == NULL || !module->activate())
			continue;

		active_modules[index] = true;
		activated++;
	}

	return activated;
}

size_t module_registered_count(module_kind_t kind) {
	size_t count = 0;

	for (size_t i = 0; i < registered_module_count; i++) {
		if (module_matches_kind(registered_modules[i], kind))
			count++;
	}

	return count;
}

size_t module_active_count(module_kind_t kind) {
	size_t count = 0;

	for (size_t i = 0; i < registered_module_count; i++) {
		if (active_modules[i] && module_matches_kind(registered_modules[i], kind))
			count++;
	}

	return count;
}

const char* module_kind_name(module_kind_t kind) {
	switch (kind) {
	case MODULE_KIND_CONSOLE:
		return "console";
	case MODULE_KIND_INPUT:
		return "input";
	case MODULE_KIND_IRQ_CONTROLLER:
		return "irq controller";
	case MODULE_KIND_TIMER:
		return "timer";
	case MODULE_KIND_STORAGE_CONTROLLER:
		return "storage controller";
	}

	return "unknown";
}
