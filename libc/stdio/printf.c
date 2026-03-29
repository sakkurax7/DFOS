#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct format_sink {
	char* buffer;
	size_t capacity;
	size_t length;
	bool failed;
} format_sink_t;

static void sink_putc(format_sink_t* sink, char c) {
	if (sink->buffer != NULL) {
		if (sink->capacity != 0 && sink->length + 1 < sink->capacity)
			sink->buffer[sink->length] = c;
	} else if (putchar((unsigned char) c) == EOF) {
		sink->failed = true;
	}

	sink->length++;
}

static void sink_write(format_sink_t* sink, const char* data, size_t length) {
	for (size_t i = 0; i < length; i++)
		sink_putc(sink, data[i]);
}

static size_t uint_to_string(unsigned int value, unsigned int base, bool upper,
		char* buffer, size_t buffer_size) {
	size_t index = 0;
	const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

	if (buffer_size == 0)
		return 0;

	if (value == 0) {
		buffer[index++] = '0';
		return index;
	}

	while (value != 0) {
		buffer[index++] = digits[value % base];
		value /= base;
	}

	for (size_t left = 0, right = index == 0 ? 0 : index - 1; left < right; left++, right--) {
		char tmp = buffer[left];
		buffer[left] = buffer[right];
		buffer[right] = tmp;
	}

	return index;
}

static int format_string(format_sink_t* sink, const char* format, va_list parameters) {
	while (*format != '\0') {
		if (*format != '%') {
			sink_putc(sink, *format++);
			continue;
		}

		format++;
		if (*format == '%') {
			sink_putc(sink, '%');
			format++;
			continue;
		}

		bool left_justify = false;
		bool zero_pad = false;
		bool alternate = false;
		char sign_char = '\0';
		unsigned int width = 0;
		char conversion;

		for (;;) {
			if (*format == '-') {
				left_justify = true;
				format++;
			} else if (*format == '0') {
				zero_pad = true;
				format++;
			} else if (*format == '#') {
				alternate = true;
				format++;
			} else if (*format == '+') {
				sign_char = '+';
				format++;
			} else if (*format == ' ') {
				if (sign_char == '\0')
					sign_char = ' ';
				format++;
			} else {
				break;
			}
		}

		while (*format >= '0' && *format <= '9') {
			width = width * 10u + (unsigned int) (*format - '0');
			format++;
		}

		conversion = *format++;
		char number_buffer[32];
		const char* text = NULL;
		size_t length = 0;
		char prefix_buffer[3];
		size_t prefix_length = 0;

		switch (conversion) {
		case 'c':
			number_buffer[0] = (char) va_arg(parameters, int);
			text = number_buffer;
			length = 1;
			break;
		case 's':
			text = va_arg(parameters, const char*);
			if (text == NULL)
				text = "(null)";
			length = strlen(text);
			break;
		case 'd':
		case 'i': {
			int value = va_arg(parameters, int);
			unsigned int magnitude;

			if (value < 0) {
				prefix_buffer[prefix_length++] = '-';
				magnitude = (unsigned int) (-(value + 1)) + 1u;
			} else {
				if (sign_char != '\0')
					prefix_buffer[prefix_length++] = sign_char;
				magnitude = (unsigned int) value;
			}

			length = uint_to_string(magnitude, 10, false, number_buffer, sizeof(number_buffer));
			text = number_buffer;
			break;
		}
		case 'u':
			length = uint_to_string(va_arg(parameters, unsigned int), 10, false,
				number_buffer, sizeof(number_buffer));
			text = number_buffer;
			break;
		case 'o':
			length = uint_to_string(va_arg(parameters, unsigned int), 8, false,
				number_buffer, sizeof(number_buffer));
			text = number_buffer;
			if (alternate && text[0] != '0')
				prefix_buffer[prefix_length++] = '0';
			break;
		case 'x':
		case 'X': {
			unsigned int value = va_arg(parameters, unsigned int);
			length = uint_to_string(value, 16, conversion == 'X',
				number_buffer, sizeof(number_buffer));
			text = number_buffer;
			if (alternate && value != 0) {
				prefix_buffer[prefix_length++] = '0';
				prefix_buffer[prefix_length++] = conversion;
			}
			break;
		}
		case 'p':
			prefix_buffer[prefix_length++] = '0';
			prefix_buffer[prefix_length++] = 'x';
			length = uint_to_string((unsigned int) va_arg(parameters, void*), 16, false,
				number_buffer, sizeof(number_buffer));
			text = number_buffer;
			break;
		default:
			sink_putc(sink, '%');
			sink_putc(sink, conversion);
			continue;
		}

		const size_t total_length = prefix_length + length;
		unsigned int padding = 0;
		const char pad_char = (zero_pad && !left_justify) ? '0' : ' ';
		if (width > total_length)
			padding = width - (unsigned int) total_length;

		if (!left_justify && pad_char == ' ') {
			for (unsigned int i = 0; i < padding; i++)
				sink_putc(sink, ' ');
		}

		if (prefix_length != 0)
			sink_write(sink, prefix_buffer, prefix_length);

		if (!left_justify && pad_char == '0') {
			for (unsigned int i = 0; i < padding; i++)
				sink_putc(sink, '0');
		}

		sink_write(sink, text, length);

		if (left_justify) {
			for (unsigned int i = 0; i < padding; i++)
				sink_putc(sink, ' ');
		}

		if (sink->failed)
			return -1;
	}

	if (sink->buffer != NULL && sink->capacity != 0) {
		size_t end = sink->length < sink->capacity ? sink->length : sink->capacity - 1;
		sink->buffer[end] = '\0';
	}

	if (sink->failed || sink->length > (size_t) INT_MAX)
		return -1;

	return (int) sink->length;
}

int vprintf(const char* restrict format, va_list parameters) {
	format_sink_t sink = { 0, 0, 0, false };
	return format_string(&sink, format, parameters);
}

int vsnprintf(char* restrict buffer, size_t buffer_size, const char* restrict format,
		va_list parameters) {
	format_sink_t sink = { buffer, buffer_size, 0, false };
	return format_string(&sink, format, parameters);
}

int snprintf(char* restrict buffer, size_t buffer_size, const char* restrict format, ...) {
	va_list parameters;
	va_start(parameters, format);
	int result = vsnprintf(buffer, buffer_size, format, parameters);
	va_end(parameters);
	return result;
}

int printf(const char* restrict format, ...) {
	va_list parameters;
	va_start(parameters, format);
	int result = vprintf(format, parameters);
	va_end(parameters);
	return result;
}
