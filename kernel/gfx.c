#include "kernel.h"

#define SCREEN_W 320
#define SCREEN_H 200

static uint8_t *const VGA_FB = (uint8_t *)0xA0000;
static uint8_t back_buffer[SCREEN_W * SCREEN_H];

/* --- 16-color "GUI palette" mapped onto VGA palette indices 0-15 --- */
void gfx_init(void) {
	vga_set_mode13h();

	/* Program a small custom palette (6-bit RGB per channel) for our GUI
	 * colors at indices 0-15, so gfx_color_t below maps directly. */
	vga_set_palette_color(0, 0, 0, 0);        /* 0  black */
	vga_set_palette_color(1, 0, 0, 42);       /* 1  blue */
	vga_set_palette_color(2, 0, 42, 0);       /* 2  green */
	vga_set_palette_color(3, 0, 42, 42);      /* 3  cyan */
	vga_set_palette_color(4, 42, 0, 0);       /* 4  red */
	vga_set_palette_color(5, 42, 0, 42);      /* 5  magenta */
	vga_set_palette_color(6, 42, 21, 0);      /* 6  brown */
	vga_set_palette_color(7, 42, 42, 42);     /* 7  light grey */
	vga_set_palette_color(8, 21, 21, 21);     /* 8  dark grey */
	vga_set_palette_color(9, 21, 21, 63);     /* 9  light blue */
	vga_set_palette_color(10, 21, 63, 21);    /* 10 light green */
	vga_set_palette_color(11, 21, 63, 63);    /* 11 light cyan */
	vga_set_palette_color(12, 63, 21, 21);    /* 12 light red */
	vga_set_palette_color(13, 63, 21, 63);    /* 13 light magenta */
	vga_set_palette_color(14, 63, 63, 21);    /* 14 yellow */
	vga_set_palette_color(15, 63, 63, 63);    /* 15 white */

	/* Desktop-ish extra colors */
	vga_set_palette_color(16, 8, 20, 36);     /* desktop teal-blue */
	vga_set_palette_color(17, 32, 32, 40);    /* window body grey */
	vga_set_palette_color(18, 12, 12, 16);    /* window shadow */
	vga_set_palette_color(19, 10, 30, 55);    /* titlebar active */
	vga_set_palette_color(20, 30, 30, 34);    /* titlebar inactive */
	vga_set_palette_color(21, 46, 46, 50);    /* button face */
	vga_set_palette_color(22, 58, 58, 60);    /* button highlight */
	vga_set_palette_color(23, 18, 18, 20);    /* button shadow */

	memset(back_buffer, 16, sizeof(back_buffer));
}

int gfx_width(void) { return SCREEN_W; }
int gfx_height(void) { return SCREEN_H; }

void gfx_putpixel(int x, int y, uint8_t color) {
	if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
	back_buffer[y * SCREEN_W + x] = color;
}

uint8_t gfx_getpixel(int x, int y) {
	if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return 0;
	return back_buffer[y * SCREEN_W + x];
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + w; if (x1 > SCREEN_W) x1 = SCREEN_W;
	int y1 = y + h; if (y1 > SCREEN_H) y1 = SCREEN_H;
	for (int yy = y0; yy < y1; yy++) {
		memset(&back_buffer[yy * SCREEN_W + x0], color, x1 - x0);
	}
}

void gfx_draw_rect(int x, int y, int w, int h, uint8_t color) {
	gfx_fill_rect(x, y, w, 1, color);
	gfx_fill_rect(x, y + h - 1, w, 1, color);
	gfx_fill_rect(x, y, 1, h, color);
	gfx_fill_rect(x + w - 1, y, 1, h, color);
}

void gfx_draw_hline(int x, int y, int w, uint8_t color) {
	gfx_fill_rect(x, y, w, 1, color);
}

void gfx_draw_vline(int x, int y, int h, uint8_t color) {
	gfx_fill_rect(x, y, 1, h, color);
}

void gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color) {
	int dx = x1 - x0; if (dx < 0) dx = -dx;
	int dy = y1 - y0; if (dy < 0) dy = -dy;
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx - dy;

	for (;;) {
		gfx_putpixel(x0, y0, color);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 < dx) { err += dx; y0 += sy; }
	}
}

void gfx_draw_char(int x, int y, char c, uint8_t fg) {
	const uint8_t *glyph = font8x8_get_glyph(c);
	for (int row = 0; row < 8; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < 8; col++) {
			if (bits & (0x80 >> col)) {
				gfx_putpixel(x + col, y + row, fg);
			}
		}
	}
}

void gfx_draw_char_bg(int x, int y, char c, uint8_t fg, uint8_t bg) {
	const uint8_t *glyph = font8x8_get_glyph(c);
	for (int row = 0; row < 8; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < 8; col++) {
			gfx_putpixel(x + col, y + row, (bits & (0x80 >> col)) ? fg : bg);
		}
	}
}

void gfx_draw_string(int x, int y, const char *s, uint8_t fg) {
	int cx = x;
	while (*s) {
		if (*s == '\n') {
			cx = x;
			y += 8;
		} else {
			gfx_draw_char(cx, y, *s, fg);
			cx += 8;
		}
		s++;
	}
}

void gfx_draw_string_bg(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
	int cx = x;
	while (*s) {
		if (*s == '\n') {
			cx = x;
			y += 8;
		} else {
			gfx_draw_char_bg(cx, y, *s, fg, bg);
			cx += 8;
		}
		s++;
	}
}

void gfx_flip(void) {
	memcpy(VGA_FB, back_buffer, SCREEN_W * SCREEN_H);
}
