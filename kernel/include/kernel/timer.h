#ifndef KERNEL_TIMER_H
#define KERNEL_TIMER_H

#include <stdbool.h>
#include <stdint.h>

#include <kernel/interrupts.h>

typedef struct timer_driver {
	const char* name;
	bool (*init)(uint32_t frequency_hz, interrupt_handler_t tick_handler);
	uint32_t (*frequency_hz)(void);
} timer_driver_t;

void timer_register_driver(const timer_driver_t* driver);
bool timer_initialize(uint32_t frequency_hz, interrupt_handler_t tick_handler);
uint32_t timer_frequency_hz(void);
const char* timer_driver_name(void);

#endif
