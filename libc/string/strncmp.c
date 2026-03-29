#include <string.h>

int strncmp(const char* left, const char* right, size_t count) {
	for (size_t i = 0; i < count; i++) {
		if (left[i] != right[i] || left[i] == '\0' || right[i] == '\0')
			return (unsigned char) left[i] - (unsigned char) right[i];
	}

	return 0;
}
