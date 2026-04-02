#include <stdbool.h>
#include <stddef.h>

#include <kernel/console.h>

static const console_driver_t* active_driver;

void console_register_driver(const console_driver_t* driver) {
	active_driver = driver;
}

bool console_initialize(void) {
	if (active_driver == NULL)
		return false;

	if (active_driver->init != NULL)
		return active_driver->init();

	return true;
}

void console_clear(void) {
	if (active_driver == NULL || active_driver->clear == NULL)
		return;

	active_driver->clear();
}

void console_putchar(char c) {
	if (active_driver == NULL || active_driver->putchar == NULL)
		return;

	active_driver->putchar(c);
}

void console_write(const char* data, size_t size) {
	if (active_driver == NULL)
		return;

	if (active_driver->write != NULL) {
		active_driver->write(data, size);
		return;
	}

	for (size_t i = 0; i < size; i++)
		console_putchar(data[i]);
}

void console_writestring(const char* data) {
	if (data == NULL)
		return;

	while (*data != '\0')
		console_putchar(*data++);
}

const char* console_driver_name(void) {
	if (active_driver == NULL || active_driver->name == NULL)
		return "unregistered";

	return active_driver->name;
}
