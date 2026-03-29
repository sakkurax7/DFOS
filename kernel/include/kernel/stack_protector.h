#ifndef KERNEL_STACK_PROTECTOR_H
#define KERNEL_STACK_PROTECTOR_H

#include <stdint.h>

void stack_protector_init(uint32_t seed_a, uint32_t seed_b);

#endif
