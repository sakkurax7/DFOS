#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/irq.h>
#include <kernel/timer.h>
#include <kernel/x86.h>

static uint32_t current_frequency_hz;

static bool pit_timer_init(uint32_t frequency_hz, interrupt_handler_t tick_handler) {
	if (frequency_hz == 0 || tick_handler == NULL)
		return false;

	if (!irq_register_handler(IRQ_LINE_TIMER, tick_handler))
		return false;

	const uint32_t divisor = 1193182u / frequency_hz;
	x86_outb(0x43, 0x36);
	x86_outb(0x40, (uint8_t) (divisor & 0xFF));
	x86_outb(0x40, (uint8_t) ((divisor >> 8) & 0xFF));
	irq_enable(IRQ_LINE_TIMER);
	current_frequency_hz = frequency_hz;
	return true;
}

static uint32_t pit_timer_frequency_hz(void) {
	return current_frequency_hz;
}

const timer_driver_t i386_pit_timer_driver = {
	.name = "8253/8254 PIT",
	.init = pit_timer_init,
	.frequency_hz = pit_timer_frequency_hz,
};
