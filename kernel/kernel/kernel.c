#include <kernel/tty.h>

void kernel_main(void) {
	init_vga();
	strPtr("Hello 123!\nTest 1 2 3\n4");
}
