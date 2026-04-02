#include <stdbool.h>
#include <stddef.h>

#include <kernel/input.h>

static const input_driver_t* active_driver;

void input_register_driver(const input_driver_t* driver) {
	active_driver = driver;
}

bool input_initialize(void) {
	if (active_driver == NULL)
		return false;

	if (active_driver->init != NULL)
		return active_driver->init();

	return true;
}

bool input_read_char_nonblocking(char* out) {
	if (active_driver == NULL || active_driver->read_char_nonblocking == NULL)
		return false;

	return active_driver->read_char_nonblocking(out);
}

bool input_debug_requested(void) {
	if (active_driver == NULL || active_driver->debug_requested == NULL)
		return false;

	return active_driver->debug_requested();
}

void input_clear_debug_request(void) {
	if (active_driver == NULL || active_driver->clear_debug_request == NULL)
		return;

	active_driver->clear_debug_request();
}

const char* input_driver_name(void) {
	if (active_driver == NULL || active_driver->name == NULL)
		return "unregistered";

	return active_driver->name;
}
