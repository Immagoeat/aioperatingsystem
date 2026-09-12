#include "kernel.h"

size_t strlen(const char *str) {
	size_t len = 0;
	while (str[len]) len++;
	return len;
}

int strcmp(const char *a, const char *b) {
	while (*a && (*a == *b)) { a++; b++; }
	return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
	while (n && *a && (*a == *b)) { a++; b++; n--; }
	if (n == 0) return 0;
	return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src) {
	char *orig = dst;
	while ((*dst++ = *src++));
	return orig;
}

char *strcat(char *dst, const char *src) {
	char *orig = dst;
	while (*dst) dst++;
	while ((*dst++ = *src++));
	return orig;
}

void *memset(void *dst, int val, size_t n) {
	unsigned char *p = dst;
	while (n--) *p++ = (unsigned char)val;
	return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
	unsigned char *d = dst;
	const unsigned char *s = src;
	while (n--) *d++ = *s++;
	return dst;
}

char *strtok_simple(char *str, char delim, char **saveptr) {
	char *start;
	if (str) start = str;
	else start = *saveptr;

	if (!start || *start == '\0') {
		*saveptr = NULL;
		return NULL;
	}

	while (*start == delim) start++;
	if (*start == '\0') { *saveptr = NULL; return NULL; }

	char *end = start;
	while (*end && *end != delim) end++;

	if (*end) {
		*end = '\0';
		*saveptr = end + 1;
	} else {
		*saveptr = NULL;
	}

	return start;
}
