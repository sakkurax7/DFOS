#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/irq.h>

static const irq_controller_t* active_controller;

void irq_controller_register(const irq_controller_t* controller) {
	active_controller = controller;
}

bool irq_controller_initialize(void) {
	if (active_controller == NULL)
		return false;

	if (active_controller->init != NULL)
		active_controller->init();

	return true;
}

const char* irq_controller_name(void) {
	if (active_controller == NULL || active_controller->name == NULL)
		return "unregistered";

	return active_controller->name;
}

bool irq_register_handler(irq_line_t irq, interrupt_handler_t handler) {
	if (active_controller == NULL || active_controller->vector_for_irq == NULL)
		return false;

	register_interrupt_handler(active_controller->vector_for_irq(irq), handler);
	return true;
}

void irq_enable(irq_line_t irq) {
	if (active_controller == NULL || active_controller->enable == NULL)
		return;

	active_controller->enable(irq);
}

void irq_disable(irq_line_t irq) {
	if (active_controller == NULL || active_controller->disable == NULL)
		return;

	active_controller->disable(irq);
}

bool irq_vector_to_line(uint8_t vector, irq_line_t* irq_out) {
	if (active_controller == NULL || active_controller->irq_for_vector == NULL)
		return false;

	return active_controller->irq_for_vector(vector, irq_out);
}

void irq_acknowledge_vector(uint8_t vector) {
	irq_line_t irq;
	if (!irq_vector_to_line(vector, &irq))
		return;

	if (active_controller->acknowledge != NULL)
		active_controller->acknowledge(irq);
}
