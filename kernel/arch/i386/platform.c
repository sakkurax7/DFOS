#include <kernel/module.h>
#include <kernel/platform.h>

extern const module_descriptor_t i386_pic_irq_controller_module;
extern const module_descriptor_t i386_pit_timer_module;
extern const module_descriptor_t i386_ps2_keyboard_module;
extern const module_descriptor_t i386_serial_console_module;
extern const module_descriptor_t i386_vga_console_module;

void platform_register_drivers(void) {
	module_register(&i386_vga_console_module);
	module_register(&i386_serial_console_module);
	module_register(&i386_ps2_keyboard_module);
	module_register(&i386_pic_irq_controller_module);
	module_register(&i386_pit_timer_module);

	module_activate_all(MODULE_KIND_CONSOLE);
	module_activate_best(MODULE_KIND_INPUT);
	module_activate_best(MODULE_KIND_IRQ_CONTROLLER);
	module_activate_best(MODULE_KIND_TIMER);
}
