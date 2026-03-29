#include <stdint.h>

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

static gdt_entry_t gdt[5];
static gdt_descriptor_t gdt_descriptor;

static void gdt_set_entry(int index, uint32_t base, uint32_t limit, uint8_t access,
		uint8_t granularity) {
	gdt[index].base_low = (uint16_t) (base & 0xFFFF);
	gdt[index].base_middle = (uint8_t) ((base >> 16) & 0xFF);
	gdt[index].base_high = (uint8_t) ((base >> 24) & 0xFF);
	gdt[index].limit_low = (uint16_t) (limit & 0xFFFF);
	gdt[index].granularity = (uint8_t) (((limit >> 16) & 0x0F) | (granularity & 0xF0));
	gdt[index].access = access;
}

void gdt_init(void) {
	gdt_descriptor.size = sizeof(gdt) - 1;
	gdt_descriptor.offset = (uint32_t) &gdt;

	gdt_set_entry(0, 0, 0, 0, 0);
	gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);
	gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);
	gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);
	gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);

	gdt_flush(&gdt_descriptor);
}
