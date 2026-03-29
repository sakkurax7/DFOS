#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <kernel/interrupts.h>
#include <kernel/panic.h>
#include <kernel/scheduler.h>
#include <kernel/x86.h>

#define IDT_ENTRIES 256
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1
#define PIC_EOI      0x20

typedef struct idt_entry {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t zero;
	uint8_t flags;
	uint16_t offset_high;
} __attribute__((packed)) idt_entry_t;

typedef struct idt_descriptor {
	uint16_t size;
	uint32_t offset;
} __attribute__((packed)) idt_descriptor_t;

extern void idt_load(const idt_descriptor_t* descriptor);
extern void* isr_stub_table[];

static idt_entry_t idt[IDT_ENTRIES];
static idt_descriptor_t idt_descriptor;
static interrupt_handler_t handlers[IDT_ENTRIES];

static const char* const exception_names[] = {
	"divide error", "debug", "non-maskable interrupt", "breakpoint",
	"overflow", "bound range exceeded", "invalid opcode", "device not available",
	"double fault", "coprocessor segment overrun", "invalid TSS", "segment not present",
	"stack-segment fault", "general protection fault", "page fault", "reserved",
	"x87 floating-point exception", "alignment check", "machine check", "SIMD floating-point exception",
	"virtualization exception", "control protection exception"
};

static void idt_set_gate(uint8_t vector, uint32_t handler, uint8_t flags) {
	idt[vector].offset_low = (uint16_t) (handler & 0xFFFF);
	idt[vector].selector = 0x08;
	idt[vector].zero = 0;
	idt[vector].flags = flags;
	idt[vector].offset_high = (uint16_t) ((handler >> 16) & 0xFFFF);
}

static void pic_remap(void) {
	const uint8_t pic1_mask = x86_inb(PIC1_DATA);
	const uint8_t pic2_mask = x86_inb(PIC2_DATA);

	x86_outb(PIC1_COMMAND, 0x11);
	x86_io_wait();
	x86_outb(PIC2_COMMAND, 0x11);
	x86_io_wait();
	x86_outb(PIC1_DATA, 0x20);
	x86_io_wait();
	x86_outb(PIC2_DATA, 0x28);
	x86_io_wait();
	x86_outb(PIC1_DATA, 0x04);
	x86_io_wait();
	x86_outb(PIC2_DATA, 0x02);
	x86_io_wait();
	x86_outb(PIC1_DATA, 0x01);
	x86_io_wait();
	x86_outb(PIC2_DATA, 0x01);
	x86_io_wait();

	x86_outb(PIC1_DATA, pic1_mask & (uint8_t) ~0x01);
	x86_outb(PIC2_DATA, pic2_mask | 0xFF);
}

static void pic_send_eoi(uint8_t irq) {
	if (irq >= 8)
		x86_outb(PIC2_COMMAND, PIC_EOI);
	x86_outb(PIC1_COMMAND, PIC_EOI);
}

static void pic_update_mask(uint8_t irq, bool masked) {
	const uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
	const uint8_t bit = (uint8_t) (1u << (irq % 8));
	uint8_t mask = x86_inb(port);

	if (masked)
		mask |= bit;
	else
		mask &= (uint8_t) ~bit;

	x86_outb(port, mask);
}

static interrupt_frame_t* default_exception_handler(interrupt_frame_t* frame) {
	const char* name = "unknown";
	if (frame->vector < (sizeof(exception_names) / sizeof(exception_names[0])))
		name = exception_names[frame->vector];

	if (frame->vector == 14) {
		panic("page fault at eip=%p addr=%p err=0x%x",
			(void*) frame->eip, (void*) x86_read_cr2(), frame->error_code);
	}

	panic("cpu exception %u (%s) at eip=%p err=0x%x",
		frame->vector, name, (void*) frame->eip, frame->error_code);
}

interrupt_frame_t* isr_dispatch(interrupt_frame_t* frame) {
	interrupt_handler_t handler = handlers[frame->vector];
	interrupt_frame_t* next_frame = frame;

	if (handler != NULL) {
		next_frame = handler(frame);
	} else if (frame->vector < 32) {
		next_frame = default_exception_handler(frame);
	}

	if (frame->vector >= 32 && frame->vector <= 47)
		pic_send_eoi((uint8_t) (frame->vector - 32));

	return next_frame;
}

void idt_init(void) {
	for (uint16_t vector = 0; vector < 49; vector++)
		idt_set_gate((uint8_t) vector, (uint32_t) isr_stub_table[vector], 0x8E);

	for (uint16_t vector = 49; vector < IDT_ENTRIES; vector++)
		idt_set_gate((uint8_t) vector, (uint32_t) isr_stub_table[48], 0x8E);

	idt_descriptor.size = sizeof(idt) - 1;
	idt_descriptor.offset = (uint32_t) &idt;

	pic_remap();
	idt_load(&idt_descriptor);
	register_interrupt_handler(32, scheduler_on_timer_tick);
	register_interrupt_handler(48, scheduler_on_yield);
}

void register_interrupt_handler(uint8_t vector, interrupt_handler_t handler) {
	handlers[vector] = handler;
}

void interrupts_enable(void) {
	x86_sti();
}

void interrupts_disable(void) {
	x86_cli();
}

void interrupts_irq_unmask(uint8_t irq) {
	pic_update_mask(irq, false);
}

void interrupts_irq_mask(uint8_t irq) {
	pic_update_mask(irq, true);
}
