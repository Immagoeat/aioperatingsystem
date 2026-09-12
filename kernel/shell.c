#include "kernel.h"

#define CMD_BUFFER_SIZE 256
#define MAX_ARGS 16

static char cmd_buffer[CMD_BUFFER_SIZE];

static void print_prompt(void) {
	terminal_setcolor(0x0A); /* light green */
	terminal_writestring("aurora");
	terminal_setcolor(0x0F); /* white */
	terminal_writestring(":~$ ");
}

static void read_line(char *buf, size_t max_len) {
	size_t len = 0;
	for (;;) {
		char c = keyboard_getchar_blocking();

		if (c == '\n') {
			terminal_putchar('\n');
			buf[len] = '\0';
			return;
		} else if (c == '\b') {
			if (len > 0) {
				len--;
				terminal_backspace();
			}
		} else if (len < max_len - 1) {
			buf[len++] = c;
			terminal_putchar(c);
		}
	}
}

static void cmd_help(void) {
	terminal_writestring("Available commands:\n");
	terminal_writestring("  help      - show this help message\n");
	terminal_writestring("  about     - about auroraOS\n");
	terminal_writestring("  clear     - clear the screen\n");
	terminal_writestring("  echo TXT  - print TXT back\n");
	terminal_writestring("  mem       - show memory map / totals\n");
	terminal_writestring("  uptime    - show timer ticks since boot\n");
	terminal_writestring("  color N   - set text color (0-15)\n");
	terminal_writestring("  reboot    - reboot the machine\n");
	terminal_writestring("  halt      - halt the CPU\n");
}

static void cmd_about(void) {
	terminal_setcolor(0x0B);
	terminal_writestring("\n  auroraOS - a tiny hobby kernel\n");
	terminal_setcolor(0x0F);
	terminal_writestring("  ------------------------------\n");
	terminal_writestring("  32-bit, multiboot, x86, written in C + assembly.\n");
	terminal_writestring("  Features: GDT, IDT, PIC remap, PIT timer,\n");
	terminal_writestring("  PS/2 keyboard driver, VGA text console, shell.\n\n");
}

static void reboot(void) {
	uint8_t good = 0x02;
	while (good & 0x02) good = inb(0x64);
	outb(0x64, 0xFE);
	for (;;) __asm__ volatile("hlt");
}

static uint8_t parse_uint(const char *s) {
	uint8_t val = 0;
	while (*s >= '0' && *s <= '9') {
		val = val * 10 + (*s - '0');
		s++;
	}
	return val;
}

static void dispatch(char *line) {
	char *saveptr;
	char *cmd = strtok_simple(line, ' ', &saveptr);
	if (!cmd) return;

	if (strcmp(cmd, "help") == 0) {
		cmd_help();
	} else if (strcmp(cmd, "about") == 0) {
		cmd_about();
	} else if (strcmp(cmd, "clear") == 0) {
		terminal_clear();
	} else if (strcmp(cmd, "echo") == 0) {
		char *rest = strtok_simple(NULL, '\0', &saveptr);
		if (rest) terminal_writestring(rest);
		terminal_putchar('\n');
	} else if (strcmp(cmd, "mem") == 0) {
		memory_print_map();
	} else if (strcmp(cmd, "uptime") == 0) {
		kprintf("Ticks since boot: %u (~%u sec)\n", timer_get_ticks(), timer_get_ticks() / 100);
	} else if (strcmp(cmd, "color") == 0) {
		char *arg = strtok_simple(NULL, ' ', &saveptr);
		if (arg) {
			uint8_t n = parse_uint(arg);
			terminal_setcolor(n & 0x0F);
			terminal_writestring("Color changed.\n");
		} else {
			terminal_writestring("usage: color N   (0-15)\n");
		}
	} else if (strcmp(cmd, "reboot") == 0) {
		terminal_writestring("Rebooting...\n");
		reboot();
	} else if (strcmp(cmd, "halt") == 0) {
		terminal_writestring("System halted.\n");
		__asm__ volatile("cli");
		for (;;) __asm__ volatile("hlt");
	} else if (strlen(cmd) == 0) {
		/* nothing */
	} else {
		terminal_setcolor(0x0C);
		terminal_writestring("Unknown command: ");
		terminal_writestring(cmd);
		terminal_writestring(" (type 'help')\n");
		terminal_setcolor(0x0F);
	}
}

void shell_run(void) {
	terminal_setcolor(0x0F);
	for (;;) {
		print_prompt();
		read_line(cmd_buffer, CMD_BUFFER_SIZE);
		dispatch(cmd_buffer);
	}
}
