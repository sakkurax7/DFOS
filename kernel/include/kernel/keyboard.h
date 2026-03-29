#ifndef KERNEL_KEYBOARD_H
#define KERNEL_KEYBOARD_H

#include <stdbool.h>

void keyboard_init(void);
bool keyboard_getchar_nonblocking(char* out);
bool keyboard_debug_requested(void);
void keyboard_clear_debug_request(void);

#endif
