#include "kernel.h"
#include <stdarg.h>

static void print_uint(unsigned int value, unsigned int base, bool upper) {
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

static void print_int(int value) {
	if (value < 0) {
		console_putchar('-');
		print_uint((unsigned int)(-value), 10, false);
	} else {
		print_uint((unsigned int)value, 10, false);
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
		switch (fmt[i]) {
			case 'd':
			case 'i':
				print_int(va_arg(args, int));
				break;
			case 'u':
				print_uint(va_arg(args, unsigned int), 10, false);
				break;
			case 'x':
				print_uint(va_arg(args, unsigned int), 16, false);
				break;
			case 'X':
				print_uint(va_arg(args, unsigned int), 16, true);
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
				console_putchar(fmt[i]);
				break;
		}
	}

	va_end(args);
	console_present();
}
