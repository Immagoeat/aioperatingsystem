/* mouse.c - basic PS/2 mouse driver (IRQ12). Standard 3-byte packet
 * mode, upgraded to the IntelliMouse 4-byte mode (real scroll wheel
 * support) when the device supports it - see mouse_install()'s magic
 * sample-rate sequence below, the real, standard way every OS
 * (including this one, now) detects and enables it. Falls back to
 * plain 3-byte packets with no wheel on hardware/emulators that don't
 * support the extension, rather than assuming it's always there. */
#include "kernel.h"

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_CMD 0x64

static int32_t mouse_x = 160;
static int32_t mouse_y = 100;
static uint8_t mouse_buttons = 0;
static int screen_w = 320;
static int screen_h = 200;

static bool mouse_has_wheel = false;
static uint8_t packet[4];
static int packet_index = 0;
static int packet_size = 3; /* 4 once mouse_has_wheel is confirmed */

static volatile bool mouse_dirty = false;
static volatile int mouse_wheel_delta = 0; /* accumulates until read via mouse_get_wheel_delta() */

static void mouse_wait_write(void) {
	uint32_t timeout = 100000;
	while (timeout--) {
		if ((inb(PS2_STATUS) & 0x02) == 0) return;
	}
}

static void mouse_wait_read(void) {
	uint32_t timeout = 100000;
	while (timeout--) {
		if (inb(PS2_STATUS) & 0x01) return;
	}
}

static void mouse_write(uint8_t data) {
	mouse_wait_write();
	outb(PS2_CMD, 0xD4); /* tell controller: next byte goes to mouse */
	mouse_wait_write();
	outb(PS2_DATA, data);
}

static uint8_t mouse_read(void) {
	mouse_wait_read();
	return inb(PS2_DATA);
}

static void mouse_callback(struct registers *regs) {
	(void)regs;
	uint8_t status = inb(PS2_STATUS);
	if (!(status & 0x20)) {
		/* Byte came from the keyboard, not the mouse; ignore here. */
		(void)inb(PS2_DATA);
		return;
	}

	uint8_t data = inb(PS2_DATA);

	/* Checked as soon as byte 0 of a new packet arrives (bit 3 must
	 * always be set in a real first byte), not just after the whole
	 * packet has accumulated - catches a misaligned stream one byte
	 * earlier than waiting until the buffer fills, so a single
	 * genuinely-bad byte can't shift every packet after it by one. */
	if (packet_index == 0 && !(data & 0x08)) return;

	packet[packet_index++] = data;

	if (packet_index == packet_size) {
		packet_index = 0;

		uint8_t flags = packet[0];

		int dx = packet[1];
		int dy = packet[2];
		if (flags & 0x10) dx -= 256; /* sign extend */
		if (flags & 0x20) dy -= 256;

		mouse_x += dx;
		mouse_y -= dy; /* PS/2 Y is inverted vs screen coordinates */

		if (mouse_x < 0) mouse_x = 0;
		if (mouse_y < 0) mouse_y = 0;
		if (mouse_x >= screen_w) mouse_x = screen_w - 1;
		if (mouse_y >= screen_h) mouse_y = screen_h - 1;

		if (mouse_has_wheel) {
			/* IntelliMouse's 4th byte: a signed 8-bit wheel delta (only
			 * the low bits are meaningful in the plain wheel-only
			 * extension this driver enables - no 4th/5th button bits are
			 * read, since nothing here would use them). Accumulated
			 * rather than overwritten so quick successive scroll ticks
			 * between two mouse_get_wheel_delta() calls both count,
			 * matching how mouse_dirty already accumulates across reads. */
			int8_t wheel = (int8_t)packet[3];
			mouse_wheel_delta += wheel;
		}

		mouse_buttons = flags & 0x07;
		mouse_dirty = true;
	}
}

void mouse_install(void) {
	/* Enable auxiliary device (mouse) */
	mouse_wait_write();
	outb(PS2_CMD, 0xA8);

	/* Enable IRQ12 in the controller configuration byte */
	mouse_wait_write();
	outb(PS2_CMD, 0x20); /* read config byte */
	uint8_t status = mouse_read();
	status |= 0x02;   /* enable IRQ12 */
	status &= ~0x20;  /* enable mouse clock */
	mouse_wait_write();
	outb(PS2_CMD, 0x60); /* write config byte */
	mouse_wait_write();
	outb(PS2_DATA, status);

	/* Use default settings */
	mouse_write(0xF6);
	(void)mouse_read(); /* ACK */

	/* IntelliMouse (scroll wheel) detection: the standard magic
	 * sequence every OS uses - set the sample rate to 200, then 100,
	 * then 80 (0xF3 is "set sample rate", each followed by the rate
	 * byte and an ACK for each of the two bytes sent). A real
	 * IntelliMouse-compatible device (real hardware or, as here,
	 * QEMU's emulated PS/2 mouse) responds to this specific sequence
	 * by switching itself into 4-byte packet mode; a plain 3-byte-only
	 * mouse just ignores the sequence as three ordinary sample-rate
	 * changes and keeps sending 3-byte packets, which is exactly what
	 * mouse_has_wheel staying false falls back to. Checked via 0xF2
	 * ("get device ID"): 0x00 is a plain mouse, 0x03 is IntelliMouse. */
	{
		const uint8_t rates[3] = { 200, 100, 80 };
		for (int i = 0; i < 3; i++) {
			mouse_write(0xF3);
			(void)mouse_read(); /* ACK for the command byte */
			mouse_write(rates[i]);
			(void)mouse_read(); /* ACK for the rate byte */
		}
		mouse_write(0xF2);
		(void)mouse_read(); /* ACK */
		uint8_t device_id = mouse_read();
		mouse_has_wheel = (device_id == 0x03);
		packet_size = mouse_has_wheel ? 4 : 3;
	}

	/* Enable data reporting */
	mouse_write(0xF4);
	(void)mouse_read(); /* ACK */

	register_interrupt_handler(44, mouse_callback); /* IRQ12 -> vector 44 */
}

int mouse_get_wheel_delta(void) {
	int delta = mouse_wheel_delta;
	mouse_wheel_delta = 0;
	return delta;
}

void mouse_set_bounds(int w, int h) {
	screen_w = w;
	screen_h = h;
	if (mouse_x >= w) mouse_x = w - 1;
	if (mouse_y >= h) mouse_y = h - 1;
}

void mouse_get_state(int *x, int *y, uint8_t *buttons) {
	*x = mouse_x;
	*y = mouse_y;
	*buttons = mouse_buttons;
}

bool mouse_poll_dirty(void) {
	if (mouse_dirty) {
		mouse_dirty = false;
		return true;
	}
	return false;
}
