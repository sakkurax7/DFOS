#include <stdint.h>
#include <string.h>

#include <kernel/gdt.h>

typedef struct gdt_entry {
	uint16_t limit_low;
	uint16_t base_low;
	uint8_t base_middle;
	uint8_t access;
	uint8_t granularity;
	uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct gdt_descriptor {
	uint16_t size;
	uint32_t offset;
} __attribute__((packed)) gdt_descriptor_t;

extern void gdt_flush(const gdt_descriptor_t* descriptor);

typedef struct tss_entry {
	uint32_t prev_tss;
	uint32_t esp0;
	uint32_t ss0;
	uint32_t esp1;
	uint32_t ss1;
	uint32_t esp2;
	uint32_t ss2;
	uint32_t cr3;
	uint32_t eip;
	uint32_t eflags;
	uint32_t eax;
	uint32_t ecx;
	uint32_t edx;
	uint32_t ebx;
	uint32_t esp;
	uint32_t ebp;
	uint32_t esi;
	uint32_t edi;
	uint32_t es;
	uint32_t cs;
	uint32_t ss;
	uint32_t ds;
	uint32_t fs;
	uint32_t gs;
	uint32_t ldt_selector;
	uint16_t trap;
	uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

static gdt_entry_t gdt[6];
static gdt_descriptor_t gdt_descriptor;
static tss_entry_t tss;

static void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access,
		uint8_t granularity) {
	gdt[index].base_low = (uint16_t) (base & 0xFFFF);
	gdt[index].base_middle = (uint8_t) ((base >> 16) & 0xFF);
	gdt[index].base_high = (uint8_t) ((base >> 24) & 0xFF);
	gdt[index].limit_low = (uint16_t) (limit & 0xFFFF);
	gdt[index].granularity = (uint8_t) (((limit >> 16) & 0x0F) | (granularity & 0xF0));
	gdt[index].access = access;
}

void gdt_set_kernel_stack(uint32_t stack_top) {
	tss.esp0 = stack_top;
}

void gdt_init(void) {
	gdt_descriptor.size = sizeof(gdt) - 1;
	gdt_descriptor.offset = (uint32_t) &gdt;

	gdt_set_entry(0, 0, 0, 0, 0);
	gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);
	gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);
	gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);
	gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);
	memset(&tss, 0, sizeof(tss));
	tss.ss0 = GDT_SELECTOR_KERNEL_DATA;
	tss.iomap_base = sizeof(tss);
	gdt_set_entry(5, (uint32_t) &tss, sizeof(tss) - 1, 0x89, 0x00);

	gdt_flush(&gdt_descriptor);

	uint16_t tss_selector = GDT_SELECTOR_TSS;
	asm volatile("ltr %0" : : "rm"(tss_selector));
}
