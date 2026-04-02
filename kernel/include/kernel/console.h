#ifndef KERNEL_CONSOLE_H
#define KERNEL_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct console_driver {
	const char* name;
	bool (*init)(void);
	void (*clear)(void);
	void (*putchar)(char c);
	void (*write)(const char* data, size_t size);
} console_driver_t;

void console_register_driver(const console_driver_t* driver);
bool console_initialize(void);
void console_clear(void);
void console_putchar(char c);
void console_write(const char* data, size_t size);
void console_writestring(const char* data);
const char* console_driver_name(void);

#endif
