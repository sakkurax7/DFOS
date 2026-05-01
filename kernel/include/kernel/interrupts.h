#ifndef KERNEL_INTERRUPTS_H
#define KERNEL_INTERRUPTS_H

#include <stdint.h>

typedef struct interrupt_frame {
	uint32_t edi;
	uint32_t esi;
	uint32_t ebp;
	uint32_t esp;
	uint32_t ebx;
	uint32_t edx;
	uint32_t ecx;
	uint32_t eax;
	uint32_t vector;
	uint32_t error_code;
	uint32_t eip;
	uint32_t cs;
	uint32_t eflags;
} interrupt_frame_t;

typedef interrupt_frame_t* (*interrupt_handler_t)(interrupt_frame_t* frame);

void idt_init(void);
void idt_load_current_cpu(void);
void interrupts_enable(void);
void interrupts_disable(void);
void register_interrupt_handler(uint8_t vector, interrupt_handler_t handler);

#endif
