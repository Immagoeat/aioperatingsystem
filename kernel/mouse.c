/* mouse.c - basic PS/2 mouse driver (IRQ12), standard 3-byte packet mode. */
#include "kernel.h"

#define PS2_DATA 0x60
#define PS2_STATUS 0x64
#define PS2_CMD 0x64

static int32_t mouse_x = 160;
static int32_t mouse_y = 100;
static uint8_t mouse_buttons = 0;
static int screen_w = 320;
static int screen_h = 200;

static uint8_t packet[3];
static int packet_index = 0;

static volatile bool mouse_dirty = false;

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
	packet[packet_index++] = data;

	if (packet_index == 3) {
		packet_index = 0;

		uint8_t flags = packet[0];
		if (!(flags & 0x08)) {
			/* Not a valid first byte (bit 3 should always be 1); resync. */
			return;
		}

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

	/* Enable data reporting */
	mouse_write(0xF4);
	(void)mouse_read(); /* ACK */

	register_interrupt_handler(44, mouse_callback); /* IRQ12 -> vector 44 */
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
