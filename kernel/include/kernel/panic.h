#ifndef KERNEL_PANIC_H
#define KERNEL_PANIC_H

#include <stdbool.h>
#include <stdint.h>

#define PANIC_LOG_MESSAGE_MAX 160u
#define PANIC_LOG_TASK_NAME_MAX 24u

typedef struct panic_record {
	uint32_t sequence;
	uint32_t tick;
	uint32_t task_id;
	char task_name[PANIC_LOG_TASK_NAME_MAX];
	char message[PANIC_LOG_MESSAGE_MAX];
	bool truncated;
} panic_record_t;

void panic_log_capture(const char* format, ...);
uint32_t panic_log_count(void);
bool panic_log_read(uint32_t newest_index, panic_record_t* record);
void panic_log_clear(void);

__attribute__((__noreturn__))
void panic(const char* format, ...);

#endif
