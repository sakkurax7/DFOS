#include <stdbool.h>
#include <stdint.h>

#include <kernel/irq.h>
#include <kernel/module.h>
#include <kernel/x86.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20
#define PIC_BASE_VECTOR 0x20

static uint16_t pic_mask = 0xFFFFu;

static void pic_write_mask(void) {
	x86_outb(PIC1_DATA, (uint8_t) (pic_mask & 0xFFu));
	x86_outb(PIC2_DATA, (uint8_t) ((pic_mask >> 8) & 0xFFu));
}

static void pic_refresh_cascade_mask(void) {
	if ((pic_mask & 0xFF00u) != 0xFF00u)
		pic_mask &= (uint16_t) ~(1u << 2);
	else
		pic_mask |= (uint16_t) (1u << 2);
}

static void pic_set_mask(irq_line_t irq, bool masked) {
	if (irq >= 16)
		return;

	if (masked)
		pic_mask |= (uint16_t) (1u << irq);
	else
		pic_mask &= (uint16_t) ~(1u << irq);

	if (irq >= 8)
		pic_refresh_cascade_mask();

	pic_write_mask();
}

static void pic_init(void) {
	x86_outb(PIC1_COMMAND, 0x11);
	x86_io_wait();
	x86_outb(PIC2_COMMAND, 0x11);
	x86_io_wait();
	x86_outb(PIC1_DATA, PIC_BASE_VECTOR);
	x86_io_wait();
	x86_outb(PIC2_DATA, PIC_BASE_VECTOR + 8);
	x86_io_wait();
	x86_outb(PIC1_DATA, 0x04);
	x86_io_wait();
	x86_outb(PIC2_DATA, 0x02);
	x86_io_wait();
	x86_outb(PIC1_DATA, 0x01);
	x86_io_wait();
	x86_outb(PIC2_DATA, 0x01);
	x86_io_wait();

	pic_mask = 0xFFFFu;
	pic_write_mask();
}

static void pic_enable_irq(irq_line_t irq) {
	pic_set_mask(irq, false);
}

static void pic_disable_irq(irq_line_t irq) {
	pic_set_mask(irq, true);
}

static void pic_acknowledge(irq_line_t irq) {
	if (irq >= 8)
		x86_outb(PIC2_COMMAND, PIC_EOI);
	x86_outb(PIC1_COMMAND, PIC_EOI);
}

static uint8_t pic_vector_for_irq(irq_line_t irq) {
	return (uint8_t) (PIC_BASE_VECTOR + irq);
}

static bool pic_irq_for_vector(uint8_t vector, irq_line_t* irq_out) {
	if (vector < PIC_BASE_VECTOR || vector >= PIC_BASE_VECTOR + 16)
		return false;

	*irq_out = (irq_line_t) (vector - PIC_BASE_VECTOR);
	return true;
}

const irq_controller_t i386_pic_irq_controller = {
	.name = "8259 PIC",
	.init = pic_init,
	.enable = pic_enable_irq,
	.disable = pic_disable_irq,
	.acknowledge = pic_acknowledge,
	.vector_for_irq = pic_vector_for_irq,
	.irq_for_vector = pic_irq_for_vector,
};

static bool pic_activate(void) {
	irq_controller_register(&i386_pic_irq_controller);
	return true;
}

const module_descriptor_t i386_pic_irq_controller_module = {
	.name = "8259 PIC",
	.kind = MODULE_KIND_IRQ_CONTROLLER,
	.priority = 50u,
	.probe = NULL,
	.activate = pic_activate,
};
