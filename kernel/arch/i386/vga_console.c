#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/console.h>
#include <kernel/module.h>

#include "vga.h"

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;
static uint16_t* const VGA_MEMORY = (uint16_t*) 0xC03FF000;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static uint16_t* terminal_buffer;

static void terminal_scroll(void) {
	for (size_t y = 1; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			terminal_buffer[(y - 1) * VGA_WIDTH + x] =
				terminal_buffer[y * VGA_WIDTH + x];
		}
	}

	for (size_t x = 0; x < VGA_WIDTH; x++)
		terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
}

static void vga_console_clear(void) {
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			const size_t index = y * VGA_WIDTH + x;
			terminal_buffer[index] = vga_entry(' ', terminal_color);
		}
	}
	terminal_row = 0;
	terminal_column = 0;
}

static bool vga_console_init(void) {
	terminal_row = 0;
	terminal_column = 0;
	terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
	terminal_buffer = VGA_MEMORY;
	vga_console_clear();
	return true;
}

static void terminal_putentryat(unsigned char c, uint8_t color, size_t x, size_t y) {
	const size_t index = y * VGA_WIDTH + x;
	terminal_buffer[index] = vga_entry(c, color);
}

static void vga_console_putchar(char c) {
	if (c == '\b') {
		if (terminal_column > 0) {
			terminal_column--;
		} else if (terminal_row > 0) {
			terminal_row--;
			terminal_column = VGA_WIDTH - 1;
		}
		terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
		return;
	}

	if (c == '\n') {
		terminal_column = 0;
		if (++terminal_row == VGA_HEIGHT) {
			terminal_row = VGA_HEIGHT - 1;
			terminal_scroll();
		}
		return;
	}

	terminal_putentryat((unsigned char) c, terminal_color, terminal_column, terminal_row);
	if (++terminal_column == VGA_WIDTH) {
		terminal_column = 0;
		if (++terminal_row == VGA_HEIGHT) {
			terminal_row = VGA_HEIGHT - 1;
			terminal_scroll();
		}
	}
}

static void vga_console_write(const char* data, size_t size) {
	for (size_t i = 0; i < size; i++)
		vga_console_putchar(data[i]);
}

const console_driver_t i386_vga_console_driver = {
	.name = "VGA text console",
	.init = vga_console_init,
	.clear = vga_console_clear,
	.putchar = vga_console_putchar,
	.write = vga_console_write,
};

static bool vga_console_activate(void) {
	console_register_driver(&i386_vga_console_driver);
	return true;
}

const module_descriptor_t i386_vga_console_module = {
	.name = "VGA text console",
	.kind = MODULE_KIND_CONSOLE,
	.priority = 100u,
	.probe = NULL,
	.activate = vga_console_activate,
};
