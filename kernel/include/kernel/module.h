#ifndef KERNEL_MODULE_H
#define KERNEL_MODULE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum module_kind {
	MODULE_KIND_CONSOLE = 0,
	MODULE_KIND_INPUT,
	MODULE_KIND_IRQ_CONTROLLER,
	MODULE_KIND_TIMER,
	MODULE_KIND_STORAGE_CONTROLLER,
} module_kind_t;

typedef struct module_descriptor {
	const char* name;
	module_kind_t kind;
	uint32_t priority;
	bool (*probe)(void);
	bool (*activate)(void);
} module_descriptor_t;

void module_register(const module_descriptor_t* module);
bool module_activate_best(module_kind_t kind);
size_t module_activate_all(module_kind_t kind);
size_t module_registered_count(module_kind_t kind);
size_t module_active_count(module_kind_t kind);
const char* module_kind_name(module_kind_t kind);

#endif
