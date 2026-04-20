#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kernel/console.h>
#include <kernel/cpu.h>
#include <kernel/interrupts.h>
#include <kernel/panic.h>
#include <kernel/scheduler.h>
#include <kernel/x86.h>

#define PANIC_LOG_CAPACITY 8u
#define X86_EFLAGS_INTERRUPT_FLAG (1u << 9)

typedef struct panic_format_buffer {
	char* buffer;
	size_t capacity;
	size_t length;
	bool truncated;
} panic_format_buffer_t;

static panic_record_t panic_log_records[PANIC_LOG_CAPACITY];
static uint32_t panic_log_next_slot;
static uint32_t panic_log_record_count;
static uint32_t panic_log_sequence;

static void panic_write_char(char c) {
	console_putchar(c);
}

static void panic_write_string(const char* text) {
	while (*text != '\0')
		panic_write_char(*text++);
}

static void panic_write_unsigned(uint32_t value, uint32_t base, bool prefix) {
	char buffer[16];
	size_t index = 0;
	const char* digits = "0123456789abcdef";

	if (prefix && base == 16)
		panic_write_string("0x");

	if (value == 0) {
		panic_write_char('0');
		return;
	}

	while (value != 0) {
		buffer[index++] = digits[value % base];
		value /= base;
	}

	while (index > 0)
		panic_write_char(buffer[--index]);
}

static uint32_t panic_irq_save(void) {
	const uint32_t flags = x86_read_eflags();
	x86_cli();
	return flags;
}

static void panic_irq_restore(uint32_t flags) {
	if ((flags & X86_EFLAGS_INTERRUPT_FLAG) != 0)
		x86_sti();
}

static void panic_format_append_char(panic_format_buffer_t* state, char c) {
	if (state == NULL || state->capacity == 0)
		return;

	if (state->length + 1u >= state->capacity) {
		state->truncated = true;
		return;
	}

	state->buffer[state->length++] = c;
	state->buffer[state->length] = '\0';
}

static void panic_format_append_string(panic_format_buffer_t* state, const char* text) {
	if (text == NULL)
		text = "(null)";

	while (*text != '\0')
		panic_format_append_char(state, *text++);
}

static void panic_format_append_unsigned(panic_format_buffer_t* state, uint32_t value,
	uint32_t base, bool prefix) {
	char buffer[16];
	size_t index = 0;
	const char* digits = "0123456789abcdef";

	if (prefix && base == 16)
		panic_format_append_string(state, "0x");

	if (value == 0) {
		panic_format_append_char(state, '0');
		return;
	}

	while (value != 0) {
		buffer[index++] = digits[value % base];
		value /= base;
	}

	while (index > 0)
		panic_format_append_char(state, buffer[--index]);
}

static void panic_format_message(char* output, size_t capacity, bool* truncated_out,
	const char* format, va_list args) {
	panic_format_buffer_t state = { output, capacity, 0, false };

	if (state.capacity > 0)
		state.buffer[0] = '\0';

	if (format == NULL)
		format = "(null panic message)";

	while (*format != '\0') {
		if (*format != '%') {
			panic_format_append_char(&state, *format++);
			continue;
		}

		format++;
		switch (*format) {
		case 's':
			panic_format_append_string(&state, va_arg(args, const char*));
			break;
		case 'u':
			panic_format_append_unsigned(&state, va_arg(args, uint32_t), 10, false);
			break;
		case 'x':
			panic_format_append_unsigned(&state, va_arg(args, uint32_t), 16, true);
			break;
		case 'p':
			panic_format_append_unsigned(
				&state, (uint32_t) va_arg(args, void*), 16, true);
			break;
		case '%':
			panic_format_append_char(&state, '%');
			break;
		case '\0':
			panic_format_append_char(&state, '%');
			break;
		default:
			panic_format_append_char(&state, '%');
			panic_format_append_char(&state, *format);
			break;
		}

		if (*format == '\0')
			break;
		format++;
	}

	if (truncated_out != NULL)
		*truncated_out = state.truncated;
}

static void panic_copy_string(char* destination, size_t capacity, const char* source) {
	if (destination == NULL || capacity == 0)
		return;

	if (source == NULL)
		source = "";

	size_t i = 0;
	for (; i + 1 < capacity && source[i] != '\0'; i++)
		destination[i] = source[i];
	destination[i] = '\0';
}

static void panic_log_append(const panic_record_t* record_template) {
	if (record_template == NULL)
		return;

	panic_record_t record = *record_template;
	const uint32_t irq_state = panic_irq_save();

	record.sequence = ++panic_log_sequence;
	panic_log_records[panic_log_next_slot] = record;
	panic_log_next_slot = (panic_log_next_slot + 1u) % PANIC_LOG_CAPACITY;
	if (panic_log_record_count < PANIC_LOG_CAPACITY)
		panic_log_record_count++;

	panic_irq_restore(irq_state);
}

static void panic_log_capture_v(const char* format, va_list args) {
	panic_record_t record;
	memset(&record, 0, sizeof(record));

	record.tick = scheduler_ticks();
	record.task_id = scheduler_current_task_id();
	const char* task_name = scheduler_current_task_name();
	if (task_name == NULL)
		task_name = "none";
	panic_copy_string(record.task_name, sizeof(record.task_name), task_name);

	panic_format_message(record.message, sizeof(record.message),
		&record.truncated, format, args);
	panic_log_append(&record);
}

void panic_log_capture(const char* format, ...) {
	va_list args;
	va_start(args, format);
	panic_log_capture_v(format, args);
	va_end(args);
}

uint32_t panic_log_count(void) {
	const uint32_t irq_state = panic_irq_save();
	const uint32_t count = panic_log_record_count;
	panic_irq_restore(irq_state);
	return count;
}

bool panic_log_read(uint32_t newest_index, panic_record_t* record) {
	if (record == NULL)
		return false;

	const uint32_t irq_state = panic_irq_save();
	if (newest_index >= panic_log_record_count) {
		panic_irq_restore(irq_state);
		return false;
	}

	const uint32_t newest_slot =
		(panic_log_next_slot + PANIC_LOG_CAPACITY - 1u) % PANIC_LOG_CAPACITY;
	const uint32_t slot =
		(newest_slot + PANIC_LOG_CAPACITY - newest_index) % PANIC_LOG_CAPACITY;
	*record = panic_log_records[slot];
	panic_irq_restore(irq_state);
	return true;
}

void panic_log_clear(void) {
	const uint32_t irq_state = panic_irq_save();
	panic_log_next_slot = 0;
	panic_log_record_count = 0;
	panic_log_sequence = 0;
	memset(panic_log_records, 0, sizeof(panic_log_records));
	panic_irq_restore(irq_state);
}

__attribute__((__noreturn__))
void panic(const char* format, ...) {
	va_list args;
	va_start(args, format);
	panic_log_capture_v(format, args);
	va_end(args);

	interrupts_disable();

	panic_record_t record;
	const bool has_record = panic_log_read(0, &record);

	panic_write_string("\nKERNEL PANIC");
	if (has_record) {
		panic_write_string(" #");
		panic_write_unsigned(record.sequence, 10, false);
	}
	panic_write_string(": ");
	panic_write_string(has_record ? record.message : "unknown panic");
	if (has_record && record.truncated)
		panic_write_string(" [truncated]");
	if (has_record) {
		panic_write_string("\ncontext: tick=");
		panic_write_unsigned(record.tick, 10, false);
		panic_write_string(" task=");
		panic_write_unsigned(record.task_id, 10, false);
		panic_write_string(" (");
		panic_write_string(record.task_name);
		panic_write_string(")");
	}
	panic_write_string("\nSystem halted.\n");

	while (true)
		cpu_halt();
}
