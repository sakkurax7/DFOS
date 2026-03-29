#include <string.h>

int strcmp(const char* left, const char* right) {
	while (*left != '\0' && *left == *right) {
		left++;
		right++;
	}

	return (unsigned char) *left - (unsigned char) *right;
}
