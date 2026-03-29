#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kernel/interrupts.h>
#include <kernel/keyboard.h>
#include <kernel/x86.h>

#define KEYBOARD_BUFFER_SIZE 256

static const char normal_map[128] = {
	0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0, 'a', 's',
	'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
	'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0
};

static const char shifted_map[128] = {
	0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
	'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0, 'A', 'S',
	'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
	'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0
};

static volatile uint32_t buffer_head;
static volatile uint32_t buffer_tail;
static char buffer[KEYBOARD_BUFFER_SIZE];
static bool shift_down;
static bool caps_lock;
static volatile bool debug_requested;

static void keyboard_push_char(char c) {
	const uint32_t next = (buffer_head + 1) % KEYBOARD_BUFFER_SIZE;
	if (next == buffer_tail)
		return;

	buffer[buffer_head] = c;
	buffer_head = next;
}

static char translate_scancode(uint8_t scancode) {
	char c = shift_down ? shifted_map[scancode] : normal_map[scancode];

	if (c >= 'a' && c <= 'z' && caps_lock)
		c = (char) (c - 'a' + 'A');
	else if (c >= 'A' && c <= 'Z' && caps_lock)
		c = (char) (c - 'A' + 'a');

	return c;
}

static interrupt_frame_t* keyboard_interrupt(interrupt_frame_t* frame) {
	const uint8_t scancode = x86_inb(0x60);

	if (scancode == 0x2A || scancode == 0x36) {
		shift_down = true;
		return frame;
	}

	if (scancode == 0xAA || scancode == 0xB6) {
		shift_down = false;
		return frame;
	}

	if (scancode == 0x3A) {
		caps_lock = !caps_lock;
		return frame;
	}

	if (scancode == 0x3B) {
		// F1 is reserved as a debugger hotkey and is not forwarded as normal input.
		debug_requested = true;
		return frame;
	}

	if ((scancode & 0x80u) != 0)
		return frame;

	if (scancode < sizeof(normal_map)) {
		const char c = translate_scancode(scancode);
		if (c != 0)
			keyboard_push_char(c);
	}

	return frame;
}

void keyboard_init(void) {
	buffer_head = 0;
	buffer_tail = 0;
	shift_down = false;
	caps_lock = false;
	debug_requested = false;
	register_interrupt_handler(33, keyboard_interrupt);
	interrupts_irq_unmask(1);
}

bool keyboard_getchar_nonblocking(char* out) {
	if (buffer_tail == buffer_head)
		return false;

	*out = buffer[buffer_tail];
	buffer_tail = (buffer_tail + 1) % KEYBOARD_BUFFER_SIZE;
	return true;
}

bool keyboard_debug_requested(void) {
	return debug_requested;
}

void keyboard_clear_debug_request(void) {
	debug_requested = false;
}
