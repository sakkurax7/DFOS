#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <kernel/console.h>

#define CONSOLE_DRIVER_CAPACITY 4
#define CONSOLE_NAME_BUFFER_SIZE 128

static const console_driver_t* registered_drivers[CONSOLE_DRIVER_CAPACITY];
static const console_driver_t* active_drivers[CONSOLE_DRIVER_CAPACITY];
static size_t registered_driver_count;
static size_t active_driver_count;
static char active_driver_names[CONSOLE_NAME_BUFFER_SIZE] = "unregistered";

static void console_driver_write(const console_driver_t* driver, const char* data, size_t size) {
	if (driver == NULL)
		return;

	if (driver->write != NULL) {
		driver->write(data, size);
		return;
	}

	if (driver->putchar == NULL)
		return;

	for (size_t i = 0; i < size; i++)
		driver->putchar(data[i]);
}

static void console_refresh_driver_names(void) {
	size_t offset = 0;

	if (active_driver_count == 0) {
		memcpy(active_driver_names, "unregistered", sizeof("unregistered"));
		return;
	}

	for (size_t i = 0; i < active_driver_count; i++) {
		const char* name = active_drivers[i]->name;
		if (name == NULL)
			name = "unnamed";

		if (i != 0) {
			if (offset + 2 >= CONSOLE_NAME_BUFFER_SIZE)
				break;
			active_driver_names[offset++] = ',';
			active_driver_names[offset++] = ' ';
		}

		while (*name != '\0' && offset + 1 < CONSOLE_NAME_BUFFER_SIZE)
			active_driver_names[offset++] = *name++;

		if (offset + 1 >= CONSOLE_NAME_BUFFER_SIZE)
			break;
	}

	active_driver_names[offset] = '\0';
}

void console_register_driver(const console_driver_t* driver) {
	if (driver == NULL || registered_driver_count >= CONSOLE_DRIVER_CAPACITY)
		return;

	registered_drivers[registered_driver_count++] = driver;
}

bool console_initialize(void) {
	active_driver_count = 0;

	for (size_t i = 0; i < registered_driver_count; i++) {
		const console_driver_t* driver = registered_drivers[i];
		bool initialized = true;

		if (driver == NULL)
			continue;

		if (driver->init != NULL)
			initialized = driver->init();

		if (!initialized || active_driver_count >= CONSOLE_DRIVER_CAPACITY)
			continue;

		active_drivers[active_driver_count++] = driver;
	}

	console_refresh_driver_names();
	return active_driver_count != 0;
}

void console_clear(void) {
	for (size_t i = 0; i < active_driver_count; i++) {
		if (active_drivers[i]->clear != NULL)
			active_drivers[i]->clear();
	}
}

void console_putchar(char c) {
	for (size_t i = 0; i < active_driver_count; i++) {
		if (active_drivers[i]->putchar != NULL)
			active_drivers[i]->putchar(c);
	}
}

void console_write(const char* data, size_t size) {
	for (size_t i = 0; i < active_driver_count; i++)
		console_driver_write(active_drivers[i], data, size);
}

void console_writestring(const char* data) {
	if (data == NULL)
		return;

	console_write(data, strlen(data));
}

const char* console_driver_name(void) {
	return active_driver_names;
}

size_t console_driver_count(void) {
	return active_driver_count;
}
