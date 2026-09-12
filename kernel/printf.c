#include "kernel.h"
#include <stdarg.h>

static void print_uint64(uint64_t value, unsigned int base, bool upper) {
	char buf[32];
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int i = 0;

	if (value == 0) {
		console_putchar('0');
		return;
	}

	while (value > 0) {
		buf[i++] = digits[value % base];
		value /= base;
	}

	while (i > 0) {
		console_putchar(buf[--i]);
	}
}

static void print_int64(int64_t value) {
	if (value < 0) {
		console_putchar('-');
		print_uint64((uint64_t)(-(value + 1)) + 1, 10, false); /* avoids UB negating INT64_MIN */
	} else {
		print_uint64((uint64_t)value, 10, false);
	}
}

void kprintf(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	for (size_t i = 0; fmt[i] != '\0'; i++) {
		if (fmt[i] != '%') {
			console_putchar(fmt[i]);
			continue;
		}

		i++;

		/* length modifier: 'l' (long) or 'll' (long long) - both treated
		 * as 64-bit here, since this is a 64-bit kernel where `long` is
		 * already 64 bits under the System V AMD64 ABI. Anything without
		 * one of these stays 32-bit, matching plain int/unsigned int. */
		bool is_64 = false;
		if (fmt[i] == 'l') {
			is_64 = true;
			i++;
			if (fmt[i] == 'l') i++; /* "ll" - still just 64-bit here */
		}

		switch (fmt[i]) {
			case 'd':
			case 'i':
				if (is_64) print_int64(va_arg(args, int64_t));
				else print_int64((int64_t)va_arg(args, int));
				break;
			case 'u':
				if (is_64) print_uint64(va_arg(args, uint64_t), 10, false);
				else print_uint64((uint64_t)va_arg(args, unsigned int), 10, false);
				break;
			case 'x':
				if (is_64) print_uint64(va_arg(args, uint64_t), 16, false);
				else print_uint64((uint64_t)va_arg(args, unsigned int), 16, false);
				break;
			case 'X':
				if (is_64) print_uint64(va_arg(args, uint64_t), 16, true);
				else print_uint64((uint64_t)va_arg(args, unsigned int), 16, true);
				break;
			case 'c':
				console_putchar((char)va_arg(args, int));
				break;
			case 's': {
				const char *s = va_arg(args, const char *);
				if (!s) s = "(null)";
				console_writestring(s);
				break;
			}
			case '%':
				console_putchar('%');
				break;
			default:
				console_putchar('%');
				if (is_64) console_writestring("ll");
				console_putchar(fmt[i]);
				break;
		}
	}

	va_end(args);
	console_present();
}
