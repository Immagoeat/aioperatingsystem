#include "kernel.h"

#define CMD_BUFFER_SIZE 256

static char cmd_buffer[CMD_BUFFER_SIZE];

static void print_prompt(void) {
	console_set_color(GFX_RGB(0x5C, 0xE1, 0x9C));
	console_writestring("aurora");
	console_set_color(console_color_default());
	console_writestring(":~$ ");
	console_present();
}

static void read_line(char *buf, size_t max_len) {
	size_t len = 0;
	for (;;) {
		char c = keyboard_getchar_blocking();

		if (c == '\n') {
			console_putchar('\n');
			buf[len] = '\0';
			console_present();
			return;
		} else if (c == '\b') {
			if (len > 0) {
				len--;
				console_putchar('\b');
			}
		} else if (len < max_len - 1) {
			buf[len++] = c;
			console_putchar(c);
		}
		console_present();
	}
}

static void cmd_help(void) {
	console_writestring("Available commands:\n");
	console_writestring("  help      - show this help message\n");
	console_writestring("  about     - about auroraOS\n");
	console_writestring("  clear     - clear the screen\n");
	console_writestring("  echo TXT  - print TXT back\n");
	console_writestring("  mem       - show memory map / totals\n");
	console_writestring("  uptime    - show timer ticks since boot\n");
	console_writestring("  gui       - launch the graphical desktop (press 'q' to exit)\n");
	console_writestring("  reboot    - reboot the machine\n");
	console_writestring("  halt      - halt the CPU\n");
}

static void cmd_about(void) {
	console_set_color(console_color_accent());
	console_writestring("\n  auroraOS - a tiny hobby kernel\n");
	console_set_color(console_color_default());
	console_writestring("  ------------------------------\n");
	console_writestring("  32-bit, multiboot, x86, written in C + assembly.\n");
	console_writestring("  Boots into a VBE linear framebuffer via GRUB.\n");
	console_writestring("  Features: GDT, IDT, PIC remap, PIT timer,\n");
	console_writestring("  PS/2 keyboard + mouse drivers, a software\n");
	console_writestring("  console, and a windowed GUI shell.\n\n");
}

static void reboot(void) {
	uint8_t good = 0x02;
	while (good & 0x02) good = inb(0x64);
	outb(0x64, 0xFE);
	for (;;) __asm__ volatile("hlt");
}

static void launch_gui(void) {
	wm_init();
	wm_run();
	console_clear();
	console_writestring("Returned to shell from GUI.\n");
}

/* Auto-boots into the GUI a few seconds after the shell starts, matching
 * how a real desktop OS behaves rather than dropping you at a bare
 * prompt. Any keypress during the countdown cancels it and drops you
 * straight into the shell instead - so the shell stays fully reachable,
 * it's just not the default anymore. */
static void auto_launch_countdown(void) {
	const uint32_t seconds = 3;
	console_writestring("Starting desktop in ");
	console_present();

	for (uint32_t remaining = seconds; remaining > 0; remaining--) {
		console_set_color(console_color_accent());
		char digit = '0' + (char)remaining;
		console_putchar(digit);
		console_set_color(console_color_default());
		console_writestring("... (press any key to cancel)");
		console_present();

		uint32_t deadline = timer_get_ticks() + 100; /* ~1 second */
		while (timer_get_ticks() < deadline) {
			if (keyboard_has_key()) {
				keyboard_getchar_blocking(); /* consume it */
				console_writestring("\nAutoboot cancelled.\n");
				console_present();
				return;
			}
			__asm__ volatile("hlt");
		}

		for (int i = 0; i < 40; i++) console_putchar('\b');
	}

	console_writestring("\n");
	console_present();
	launch_gui();
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
		console_clear();
	} else if (strcmp(cmd, "echo") == 0) {
		char *rest = strtok_simple(NULL, '\0', &saveptr);
		if (rest) console_writestring(rest);
		console_putchar('\n');
	} else if (strcmp(cmd, "mem") == 0) {
		memory_print_map();
	} else if (strcmp(cmd, "uptime") == 0) {
		kprintf("Ticks since boot: %u (~%u sec)\n", timer_get_ticks(), timer_get_ticks() / 100);
	} else if (strcmp(cmd, "gui") == 0) {
		launch_gui();
	} else if (strcmp(cmd, "reboot") == 0) {
		console_writestring("Rebooting...\n");
		console_present();
		reboot();
	} else if (strcmp(cmd, "halt") == 0) {
		console_writestring("System halted.\n");
		console_present();
		__asm__ volatile("cli");
		for (;;) __asm__ volatile("hlt");
	} else if (strlen(cmd) == 0) {
		/* nothing */
	} else {
		console_set_color(GFX_RGB(0xFF, 0x6B, 0x6B));
		console_writestring("Unknown command: ");
		console_writestring(cmd);
		console_writestring(" (type 'help')\n");
		console_set_color(console_color_default());
	}
	console_present();
}

void shell_run(void) {
	auto_launch_countdown();

	for (;;) {
		print_prompt();
		read_line(cmd_buffer, CMD_BUFFER_SIZE);
		dispatch(cmd_buffer);
	}
}
