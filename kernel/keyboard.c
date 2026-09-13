#include "kernel.h"

#define KBD_BUFFER_SIZE 256

static char kbd_buffer[KBD_BUFFER_SIZE];
static volatile int kbd_head = 0;
static volatile int kbd_tail = 0;
static bool shift_pressed = false;
static bool ctrl_pressed = false;
static bool caps_lock = false;

/* US QWERTY scancode set 1 -> ASCII (unshifted) */
static const char scancode_ascii[128] = {
	0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
	'\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
	0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
	0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
	'*', 0, ' ', 0,
};

static const char scancode_ascii_shift[128] = {
	0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
	'\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
	0, 'A','S','D','F','G','H','J','K','L',':','"','~',
	0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
	'*', 0, ' ', 0,
};

static bool extended_prefix = false; /* saw 0xE0 - the next byte is an "extended" key (arrows, etc.) */

static void kbd_buffer_push(char c) {
	int next = (kbd_head + 1) % KBD_BUFFER_SIZE;
	if (next != kbd_tail) {
		kbd_buffer[kbd_head] = c;
		kbd_head = next;
	}
}

static void keyboard_callback(struct registers *regs) {
	(void)regs;
	uint8_t scancode = inb(0x60);

	if (scancode == 0xE0) { extended_prefix = true; return; }

	if (extended_prefix) {
		extended_prefix = false;
		/* Arrow keys (scancode set 1, "extended" 0xE0-prefixed codes).
		 * Pushed as values above the ASCII/Ctrl-code range (see
		 * KEY_ARROW_* in kernel.h) so callers can tell them apart from
		 * any real character without a separate "is this an arrow key"
		 * API - keyboard_getchar_blocking() still just returns a char. */
		if (!(scancode & 0x80)) { /* ignore the key-release half */
			switch (scancode) {
				case 0x48: kbd_buffer_push(KEY_ARROW_UP); break;
				case 0x50: kbd_buffer_push(KEY_ARROW_DOWN); break;
				case 0x4B: kbd_buffer_push(KEY_ARROW_LEFT); break;
				case 0x4D: kbd_buffer_push(KEY_ARROW_RIGHT); break;
				default: break; /* other extended keys: not handled */
			}
		}
		return;
	}

	if (scancode == 0x2A || scancode == 0x36) { shift_pressed = true; return; }
	if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = false; return; }
	if (scancode == 0x1D) { ctrl_pressed = true; return; }  /* left ctrl down */
	if (scancode == 0x9D) { ctrl_pressed = false; return; } /* left ctrl up */
	if (scancode == 0x3A) { caps_lock = !caps_lock; return; }

	if (scancode & 0x80) {
		return; /* key release, ignore otherwise */
	}

	if (scancode < 128) {
		char c = shift_pressed ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
		if (caps_lock && c >= 'a' && c <= 'z') c -= 32;
		else if (caps_lock && c >= 'A' && c <= 'Z') c += 32;

		if (ctrl_pressed && c >= 'a' && c <= 'z') {
			/* Traditional Ctrl+letter encoding: Ctrl+A=1 .. Ctrl+Z=26,
			 * same convention terminals have used forever. Keeps this
			 * as a plain char so no new keyboard API is needed - callers
			 * that care about Ctrl+<key> shortcuts just compare against
			 * these values instead of the letter itself. */
			c = (char)(c - 'a' + 1);
		}

		if (c) kbd_buffer_push(c);
	}
}

void keyboard_install(void) {
	register_interrupt_handler(33, keyboard_callback);
}

char keyboard_getchar_blocking(void) {
	while (kbd_head == kbd_tail) {
		__asm__ volatile("hlt");
	}
	char c = kbd_buffer[kbd_tail];
	kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
	return c;
}

bool keyboard_has_key(void) {
	return kbd_head != kbd_tail;
}
