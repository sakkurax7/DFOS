#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <stdbool.h>

bool cpu_has_pae(void);
void cpu_halt(void);

#endif
