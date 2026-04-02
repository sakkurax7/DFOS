#include <kernel/console.h>
#include <kernel/input.h>
#include <kernel/irq.h>
#include <kernel/platform.h>
#include <kernel/timer.h>

extern const console_driver_t i386_vga_console_driver;
extern const input_driver_t i386_ps2_keyboard_driver;
extern const irq_controller_t i386_pic_irq_controller;
extern const timer_driver_t i386_pit_timer_driver;

void platform_register_drivers(void) {
	console_register_driver(&i386_vga_console_driver);
	input_register_driver(&i386_ps2_keyboard_driver);
	irq_controller_register(&i386_pic_irq_controller);
	timer_register_driver(&i386_pit_timer_driver);
}
