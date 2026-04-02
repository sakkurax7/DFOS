#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/console.h>
#include <kernel/module.h>
#include <kernel/x86.h>

#define SERIAL_COM1_BASE           0x3F8
#define SERIAL_DATA_REGISTER       0
#define SERIAL_INTERRUPT_ENABLE    1
#define SERIAL_FIFO_CONTROL        2
#define SERIAL_LINE_CONTROL        3
#define SERIAL_MODEM_CONTROL       4
#define SERIAL_LINE_STATUS         5
#define SERIAL_SCRATCH             7

#define SERIAL_LINE_DLAB           0x80
#define SERIAL_LINE_8N1            0x03
#define SERIAL_FIFO_ENABLE_CLEAR   0xC7
#define SERIAL_MODEM_DTR_RTS_OUT2  0x0B
#define SERIAL_TX_READY            0x20

static bool serial_console_available;

static uint16_t serial_register(uint16_t offset) {
	return (uint16_t) (SERIAL_COM1_BASE + offset);
}

static bool serial_console_probe(void) {
	x86_outb(serial_register(SERIAL_SCRATCH), 0x5Au);
	return x86_inb(serial_register(SERIAL_SCRATCH)) == 0x5Au;
}

static bool serial_console_init(void) {
	x86_outb(serial_register(SERIAL_INTERRUPT_ENABLE), 0x00);
	x86_outb(serial_register(SERIAL_LINE_CONTROL), SERIAL_LINE_DLAB);
	x86_outb(serial_register(SERIAL_DATA_REGISTER), 0x03);
	x86_outb(serial_register(SERIAL_INTERRUPT_ENABLE), 0x00);
	x86_outb(serial_register(SERIAL_LINE_CONTROL), SERIAL_LINE_8N1);
	x86_outb(serial_register(SERIAL_FIFO_CONTROL), SERIAL_FIFO_ENABLE_CLEAR);
	x86_outb(serial_register(SERIAL_MODEM_CONTROL), SERIAL_MODEM_DTR_RTS_OUT2);
	serial_console_available = true;
	return true;
}

static void serial_console_write_byte(uint8_t value) {
	if (!serial_console_available)
		return;

	while ((x86_inb(serial_register(SERIAL_LINE_STATUS)) & SERIAL_TX_READY) == 0)
		;

	x86_outb(serial_register(SERIAL_DATA_REGISTER), value);
}

static void serial_console_putchar(char c) {
	if (c == '\n')
		serial_console_write_byte('\r');

	serial_console_write_byte((uint8_t) c);
}

static void serial_console_write(const char* data, size_t size) {
	for (size_t i = 0; i < size; i++)
		serial_console_putchar(data[i]);
}

const console_driver_t i386_serial_console_driver = {
	.name = "16550 UART (COM1)",
	.init = serial_console_init,
	.clear = NULL,
	.putchar = serial_console_putchar,
	.write = serial_console_write,
};

static bool serial_console_activate(void) {
	console_register_driver(&i386_serial_console_driver);
	return true;
}

const module_descriptor_t i386_serial_console_module = {
	.name = "16550 UART (COM1)",
	.kind = MODULE_KIND_CONSOLE,
	.priority = 80u,
	.probe = serial_console_probe,
	.activate = serial_console_activate,
};
