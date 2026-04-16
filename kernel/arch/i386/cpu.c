#include <stdbool.h>
#include <stdint.h>

#include <kernel/cpu.h>
#include <kernel/x86.h>

bool cpu_has_pae(void) {
	return x86_cpu_has_pae();
}

uint32_t cpu_current_id(void) {
	return 0;
}

void cpu_halt(void) {
	x86_hlt();
}
