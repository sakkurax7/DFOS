#ifndef _STDIO_H
#define _STDIO_H 1

#include <stddef.h>
#include <sys/cdefs.h>
#include <stdarg.h>

#define EOF (-1)

#ifdef __cplusplus
extern "C" {
#endif

int printf(const char* __restrict, ...);
int vprintf(const char* __restrict, va_list);
int snprintf(char* __restrict, size_t, const char* __restrict, ...);
int vsnprintf(char* __restrict, size_t, const char* __restrict, va_list);
int putchar(int);
int puts(const char*);

#ifdef __cplusplus
}
#endif

#endif
