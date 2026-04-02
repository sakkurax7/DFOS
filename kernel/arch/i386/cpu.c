#include <stdbool.h>

#include <kernel/cpu.h>
#include <kernel/x86.h>

bool cpu_has_pae(void) {
	return x86_cpu_has_pae();
}

void cpu_halt(void) {
	x86_hlt();
}
