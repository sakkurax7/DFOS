#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/timer.h>

static const timer_driver_t* active_driver;

void timer_register_driver(const timer_driver_t* driver) {
	active_driver = driver;
}

bool timer_initialize(uint32_t frequency_hz, interrupt_handler_t tick_handler) {
	if (active_driver == NULL || active_driver->init == NULL)
		return false;

	return active_driver->init(frequency_hz, tick_handler);
}

uint32_t timer_frequency_hz(void) {
	if (active_driver == NULL || active_driver->frequency_hz == NULL)
		return 0;

	return active_driver->frequency_hz();
}

const char* timer_driver_name(void) {
	if (active_driver == NULL || active_driver->name == NULL)
		return "unregistered";

	return active_driver->name;
}
