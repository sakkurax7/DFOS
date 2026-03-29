#include <stdint.h>

#include <kernel/x86.h>

void pit_init(uint32_t frequency_hz) {
	const uint32_t divisor = 1193182u / frequency_hz;

	x86_outb(0x43, 0x36);
	x86_outb(0x40, (uint8_t) (divisor & 0xFF));
	x86_outb(0x40, (uint8_t) ((divisor >> 8) & 0xFF));
}
