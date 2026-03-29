#include <stdint.h>

#include <kernel/panic.h>
#include <kernel/stack_protector.h>

uintptr_t __stack_chk_guard = 0x6f2c8e51u;

void stack_protector_init(uint32_t seed_a, uint32_t seed_b) {
	uintptr_t guard = ((uintptr_t) seed_a << 16) ^ (uintptr_t) seed_b ^ 0x9e3779b9u;

	if (guard == 0)
		guard = 0x6f2c8e51u;

	__stack_chk_guard = guard;
}

__attribute__((__noreturn__))
void __stack_chk_fail(void) {
	panic("stack smash detected");
}
