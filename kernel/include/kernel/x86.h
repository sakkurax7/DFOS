#ifndef KERNEL_X86_H
#define KERNEL_X86_H

#include <stdbool.h>
#include <stdint.h>

static inline void x86_outb(uint16_t port, uint8_t value) {
	asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t x86_inb(uint16_t port) {
	uint8_t value;
	asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline void x86_io_wait(void) {
	asm volatile("outb %%al, $0x80" : : "a"(0));
}

static inline uint32_t x86_read_cr0(void) {
	uint32_t value;
	asm volatile("mov %%cr0, %0" : "=r"(value));
	return value;
}

static inline uint32_t x86_read_cr2(void) {
	uint32_t value;
	asm volatile("mov %%cr2, %0" : "=r"(value));
	return value;
}

static inline uint32_t x86_read_cr3(void) {
	uint32_t value;
	asm volatile("mov %%cr3, %0" : "=r"(value));
	return value;
}

static inline void x86_write_cr3(uint32_t value) {
	asm volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline uint32_t x86_read_cr4(void) {
	uint32_t value;
	asm volatile("mov %%cr4, %0" : "=r"(value));
	return value;
}

static inline void x86_cli(void) {
	asm volatile("cli");
}

static inline void x86_sti(void) {
	asm volatile("sti");
}

static inline void x86_hlt(void) {
	asm volatile("hlt");
}

static inline void x86_cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx,
		uint32_t* ecx, uint32_t* edx) {
	asm volatile("cpuid"
		: "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
		: "a"(leaf));
}

static inline bool x86_cpu_has_pae(void) {
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	x86_cpuid(1, &eax, &ebx, &ecx, &edx);
	(void) eax;
	(void) ebx;
	(void) ecx;
	return (edx & (1u << 6)) != 0;
}

#endif
