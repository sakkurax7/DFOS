#ifndef KERNEL_GDT_H
#define KERNEL_GDT_H

#include <stdint.h>

#define GDT_SELECTOR_KERNEL_CODE 0x08u
#define GDT_SELECTOR_KERNEL_DATA 0x10u
#define GDT_SELECTOR_USER_CODE   0x18u
#define GDT_SELECTOR_USER_DATA   0x20u
#define GDT_SELECTOR_TSS         0x28u

void gdt_init(void);
void gdt_set_kernel_stack(uint32_t stack_top);

#endif
