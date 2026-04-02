#ifndef KERNEL_INPUT_H
#define KERNEL_INPUT_H

#include <stdbool.h>

typedef struct input_driver {
	const char* name;
	bool (*init)(void);
	bool (*read_char_nonblocking)(char* out);
	bool (*debug_requested)(void);
	void (*clear_debug_request)(void);
} input_driver_t;

void input_register_driver(const input_driver_t* driver);
bool input_initialize(void);
bool input_read_char_nonblocking(char* out);
bool input_debug_requested(void);
void input_clear_debug_request(void);
const char* input_driver_name(void);

#endif
