#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <stdbool.h>
#include <stdint.h>

bool cpu_has_pae(void);
uint32_t cpu_current_id(void);
void cpu_halt(void);

#endif
