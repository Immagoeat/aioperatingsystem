/* console.c - a software text console rendered onto the graphics
 * framebuffer. Replaces the old hardware-VGA-text-mode terminal now that
 * the kernel boots straight into a VBE linear framebuffer: there's no
 * separate "text mode" to fall back to, so the shell's text UI is just
 * another thing drawn with gfx.c, with its own scrollback/cursor logic. */
#include "kernel.h"

#define CON_MARGIN 12
#define CON_LINE_SPACING 2

#define COL_BG      GFX_RGB(0x10, 0x12, 0x18)
#define COL_FG      GFX_RGB(0xE4, 0xE6, 0xEB)
#define COL_ACCENT  GFX_RGB(0x5B, 0x9C, 0xFF)
#define COL_DIM     GFX_RGB(0x6B, 0x72, 0x80)
#define COL_CURSOR  GFX_RGB(0x5B, 0x9C, 0xFF)

static int con_cols, con_rows;
static int con_x, con_y; /* cursor cell position */
static gfx_color_t con_color;
static uint32_t cursor_blink_tick;

/* one back-buffer of cell contents so we can redraw/scroll cleanly */
#define CON_MAX_COLS 220
#define CON_MAX_ROWS 90
static char cell_char[CON_MAX_ROWS][CON_MAX_COLS];
static gfx_color_t cell_color[CON_MAX_ROWS][CON_MAX_COLS];

static void con_redraw_all(void) {
	gfx_fill_rect(0, 0, gfx_width(), gfx_height(), COL_BG);
	int cw = gfx_char_width();
	int ch = gfx_char_height() + CON_LINE_SPACING;
	for (int row = 0; row < con_rows; row++) {
		for (int col = 0; col < con_cols; col++) {
			char c = cell_char[row][col];
			if (c && c != ' ') {
				gfx_draw_char(CON_MARGIN + col * cw, CON_MARGIN + row * ch, c, cell_color[row][col]);
			}
		}
	}
}

void console_init(void) {
	int cw = gfx_char_width();
	int ch = gfx_char_height() + CON_LINE_SPACING;
	con_cols = (gfx_width() - 2 * CON_MARGIN) / cw;
	con_rows = (gfx_height() - 2 * CON_MARGIN) / ch;
	if (con_cols > CON_MAX_COLS) con_cols = CON_MAX_COLS;
	if (con_rows > CON_MAX_ROWS) con_rows = CON_MAX_ROWS;

	con_x = 0;
	con_y = 0;
	con_color = COL_FG;
	memset(cell_char, 0, sizeof(cell_char));

	gfx_fill_rect(0, 0, gfx_width(), gfx_height(), COL_BG);
	gfx_flip();
}

void console_set_color(gfx_color_t color) {
	con_color = color;
}

gfx_color_t console_color_default(void) { return COL_FG; }
gfx_color_t console_color_accent(void) { return COL_ACCENT; }
gfx_color_t console_color_dim(void) { return COL_DIM; }

static void con_scroll(void) {
	for (int row = 1; row < con_rows; row++) {
		memcpy(cell_char[row - 1], cell_char[row], sizeof(cell_char[row]));
		memcpy(cell_color[row - 1], cell_color[row], sizeof(cell_color[row]));
	}
	memset(cell_char[con_rows - 1], 0, sizeof(cell_char[con_rows - 1]));
	con_redraw_all();
}

void console_clear(void) {
	memset(cell_char, 0, sizeof(cell_char));
	con_x = 0;
	con_y = 0;
	gfx_fill_rect(0, 0, gfx_width(), gfx_height(), COL_BG);
}

void console_putchar(char c) {
	int cw = gfx_char_width();
	int ch = gfx_char_height() + CON_LINE_SPACING;

	if (c == '\n') {
		con_x = 0;
		con_y++;
	} else if (c == '\r') {
		con_x = 0;
	} else if (c == '\t') {
		con_x = (con_x + 4) & ~3;
	} else if (c == '\b') {
		if (con_x > 0) {
			con_x--;
			cell_char[con_y][con_x] = 0;
			gfx_fill_rect(CON_MARGIN + con_x * cw, CON_MARGIN + con_y * ch, cw, ch, COL_BG);
		}
		return;
	} else {
		if (con_x >= con_cols) {
			con_x = 0;
			con_y++;
		}
		if (con_y >= con_rows) {
			con_scroll();
			con_y = con_rows - 1;
		}
		cell_char[con_y][con_x] = c;
		cell_color[con_y][con_x] = con_color;
		gfx_fill_rect(CON_MARGIN + con_x * cw, CON_MARGIN + con_y * ch, cw, ch, COL_BG);
		gfx_draw_char(CON_MARGIN + con_x * cw, CON_MARGIN + con_y * ch, c, con_color);
		con_x++;
		return;
	}

	if (con_x >= con_cols) {
		con_x = 0;
		con_y++;
	}
	if (con_y >= con_rows) {
		con_scroll();
		con_y = con_rows - 1;
	}
}

void console_write(const char *s, size_t len) {
	for (size_t i = 0; i < len; i++) console_putchar(s[i]);
}

void console_writestring(const char *s) {
	console_write(s, strlen(s));
}

static void draw_cursor(bool on) {
	int cw = gfx_char_width();
	int ch = gfx_char_height() + CON_LINE_SPACING;
	int px = CON_MARGIN + con_x * cw;
	int py = CON_MARGIN + con_y * ch;
	if (on) {
		gfx_blend_rect(px, py + ch - 3, cw, 2, COL_CURSOR, 255);
	} else {
		gfx_fill_rect(px, py + ch - 3, cw, 2, COL_BG);
		char existing = (con_y < con_rows && con_x < con_cols) ? cell_char[con_y][con_x] : 0;
		if (existing) gfx_draw_char(px, py, existing, cell_color[con_y][con_x]);
	}
}

void console_present(void) {
	uint32_t t = timer_get_ticks();
	bool on = ((t / 30) % 2) == 0;
	if (t != cursor_blink_tick) {
		draw_cursor(on);
		cursor_blink_tick = t;
	}
	gfx_flip();
}
