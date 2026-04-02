#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/console.h>
#include <kernel/cpu.h>
#include <kernel/interrupts.h>
#include <kernel/panic.h>

static void panic_write_char(char c) {
	console_putchar(c);
}

static void panic_write_string(const char* text) {
	while (*text != '\0')
		panic_write_char(*text++);
}

static void panic_write_unsigned(uint32_t value, uint32_t base, bool prefix) {
	char buffer[16];
	size_t index = 0;
	const char* digits = "0123456789abcdef";

	if (prefix && base == 16)
		panic_write_string("0x");

	if (value == 0) {
		panic_write_char('0');
		return;
	}

	while (value != 0) {
		buffer[index++] = digits[value % base];
		value /= base;
	}

	while (index > 0)
		panic_write_char(buffer[--index]);
}

__attribute__((__noreturn__))
void panic(const char* format, ...) {
	va_list args;
	va_start(args, format);

	interrupts_disable();
	panic_write_string("\nKERNEL PANIC: ");

	while (*format != '\0') {
		if (*format != '%') {
			panic_write_char(*format++);
			continue;
		}

		format++;
		switch (*format) {
		case 's':
			panic_write_string(va_arg(args, const char*));
			break;
		case 'u':
			panic_write_unsigned(va_arg(args, uint32_t), 10, false);
			break;
		case 'x':
			panic_write_unsigned(va_arg(args, uint32_t), 16, true);
			break;
		case 'p':
			panic_write_unsigned((uint32_t) va_arg(args, void*), 16, true);
			break;
		default:
			panic_write_char('%');
			panic_write_char(*format);
			break;
		}
		format++;
	}

	va_end(args);
	panic_write_string("\nSystem halted.\n");

	while (true)
		cpu_halt();
}
