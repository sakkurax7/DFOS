#ifndef KERNEL_IRQ_H
#define KERNEL_IRQ_H

#include <stdbool.h>
#include <stdint.h>

#include <kernel/interrupts.h>

typedef uint8_t irq_line_t;

enum {
	IRQ_LINE_TIMER = 0,
	IRQ_LINE_KEYBOARD = 1,
};

typedef struct irq_controller {
	const char* name;
	void (*init)(void);
	void (*enable)(irq_line_t irq);
	void (*disable)(irq_line_t irq);
	void (*acknowledge)(irq_line_t irq);
	uint8_t (*vector_for_irq)(irq_line_t irq);
	bool (*irq_for_vector)(uint8_t vector, irq_line_t* irq_out);
} irq_controller_t;

void irq_controller_register(const irq_controller_t* controller);
bool irq_controller_initialize(void);
const char* irq_controller_name(void);
bool irq_register_handler(irq_line_t irq, interrupt_handler_t handler);
void irq_enable(irq_line_t irq);
void irq_disable(irq_line_t irq);
void irq_acknowledge_vector(uint8_t vector);
bool irq_vector_to_line(uint8_t vector, irq_line_t* irq_out);

#endif
