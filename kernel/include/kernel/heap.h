#ifndef KERNEL_HEAP_H
#define KERNEL_HEAP_H

#include <stddef.h>

void heap_init(void);
void* kmalloc(size_t size);
void* kzalloc(size_t size);
void kfree(void* ptr);
void* heap_start(void);
void* heap_end(void);

#endif
