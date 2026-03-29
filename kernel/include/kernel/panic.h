#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

__attribute__((__noreturn__))
void panic(const char* format, ...);

#endif
