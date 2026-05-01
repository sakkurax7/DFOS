#ifndef KERNEL_CPU_H
#define KERNEL_CPU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct scheduler_topology_config scheduler_topology_config_t;

bool cpu_has_pae(void);
uint32_t cpu_current_id(void);
bool cpu_smp_available(void);
bool cpu_smp_prepare_topology(scheduler_topology_config_t* config);
bool cpu_smp_start_secondary_cores(void);
void cpu_smp_release_secondary_cores(void);
void cpu_send_reschedule_ipi(uint32_t cpu_id);
void cpu_send_wakeup_ipi(uint32_t cpu_id);
void cpu_halt(void);

#endif
