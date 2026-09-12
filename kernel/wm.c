/* wm.c - a small modern-flat-design windowing compositor: draggable
 * windows with soft shadows and rounded corners, a translucent-look
 * taskbar, a desktop background, and a few demo apps. Runs entirely on
 * the true-color linear framebuffer set up by gfx.c. */
#include "kernel.h"

#define MAX_WINDOWS 8
#define TITLEBAR_H 34
#define TASKBAR_H 48
#define CORNER_RADIUS 10
#define PADDING 16

/* --- modern flat palette --- */
#define COL_DESKTOP_TOP    GFX_RGB(0x2B, 0x3A, 0x67)
#define COL_DESKTOP_BOTTOM GFX_RGB(0x16, 0x1D, 0x3B)
#define COL_WIN_BODY       GFX_RGB(0x24, 0x27, 0x33)
#define COL_WIN_BODY_ALT   GFX_RGB(0x2A, 0x2E, 0x3B)
#define COL_TITLE_ACT      GFX_RGB(0x33, 0x3A, 0x4D)
#define COL_TITLE_INACT    GFX_RGB(0x27, 0x29, 0x33)
#define COL_ACCENT         GFX_RGB(0x5B, 0x9C, 0xFF)
#define COL_ACCENT_DIM     GFX_RGB(0x3E, 0x5C, 0x8F)
#define COL_TEXT           GFX_RGB(0xEC, 0xEE, 0xF2)
#define COL_TEXT_DIM       GFX_RGB(0x9A, 0xA0, 0xAE)
#define COL_CLOSE          GFX_RGB(0xFF, 0x5F, 0x57)
#define COL_MIN            GFX_RGB(0xFF, 0xBD, 0x2E)
#define COL_MAX            GFX_RGB(0x28, 0xC8, 0x40)
#define COL_TASKBAR        GFX_RGB(0x14, 0x16, 0x22)
#define COL_TASKBAR_ACTIVE GFX_RGB(0x30, 0x36, 0x4A)
#define COL_WHITE          GFX_RGB(0xFF, 0xFF, 0xFF)
#define COL_BLACK          GFX_RGB(0x00, 0x00, 0x00)

struct window;
typedef void (*window_paint_fn)(struct window *w);

struct window {
	bool used;
	int x, y, w, h;
	char title[32];
	window_paint_fn paint;
	int counter;
	gfx_color_t accent;
};

static struct window windows[MAX_WINDOWS];
static int window_order[MAX_WINDOWS];
static int window_count = 0;

static int dragging_window = -1;
static int drag_offset_x, drag_offset_y;

static int screen_w, screen_h;

static void raise_window(int idx) {
	int pos = -1;
	for (int i = 0; i < window_count; i++) {
		if (window_order[i] == idx) { pos = i; break; }
	}
	if (pos < 0) return;
	for (int i = pos; i < window_count - 1; i++) {
		window_order[i] = window_order[i + 1];
	}
	window_order[window_count - 1] = idx;
}

static int topmost_window_at(int x, int y) {
	for (int i = window_count - 1; i >= 0; i--) {
		int idx = window_order[i];
		struct window *w = &windows[idx];
		if (!w->used) continue;
		if (x >= w->x && x < w->x + w->w &&
		    y >= w->y && y < w->y + w->h + TITLEBAR_H) {
			return idx;
		}
	}
	return -1;
}

/* --- demo apps --- */
static void paint_about(struct window *w) {
	int x = w->x + PADDING, y = w->y + TITLEBAR_H + PADDING;
	int lh = gfx_char_height() + 6;
	gfx_draw_string(x, y, "auroraOS", w->accent); y += lh + 4;
	gfx_draw_string(x, y, "A tiny hobby kernel with a", COL_TEXT); y += lh;
	gfx_draw_string(x, y, "real graphical desktop.", COL_TEXT); y += lh + 8;
	gfx_draw_string(x, y, "Drag windows by their", COL_TEXT_DIM); y += lh;
	gfx_draw_string(x, y, "title bar. Click a taskbar", COL_TEXT_DIM); y += lh;
	gfx_draw_string(x, y, "icon to focus a window.", COL_TEXT_DIM); y += lh;
	gfx_draw_string(x, y, "Press 'q' for the shell.", COL_TEXT_DIM);
}

static void paint_counter(struct window *w) {
	int x = w->x + PADDING, y = w->y + TITLEBAR_H + PADDING;
	int lh = gfx_char_height() + 6;
	gfx_draw_string(x, y, "System uptime", COL_TEXT_DIM); y += lh + 4;

	char buf[16];
	uint32_t val = (uint32_t)w->counter;
	int i = 15;
	buf[i] = '\0';
	if (val == 0) buf[--i] = '0';
	while (val > 0 && i > 0) {
		buf[--i] = '0' + (val % 10);
		val /= 10;
	}
	gfx_draw_string(x, y, &buf[i], w->accent);
	gfx_draw_string(x + gfx_string_width(&buf[i]) + 8, y, "ticks", COL_TEXT_DIM);
	y += lh + 12;

	int barw = w->w - 2 * PADDING;
	int fill = (w->counter * 2) % (barw * 2);
	if (fill > barw) fill = 2 * barw - fill; /* ping-pong */
	gfx_fill_round_rect(x, y, barw, 10, 5, COL_WIN_BODY_ALT);
	if (fill > 4) gfx_fill_round_rect(x, y, fill, 10, 5, w->accent);
}

static void paint_palette(struct window *w) {
	static const gfx_color_t swatches[8] = {
		GFX_RGB(0xFF, 0x5F, 0x57), GFX_RGB(0xFF, 0xBD, 0x2E),
		GFX_RGB(0x28, 0xC8, 0x40), GFX_RGB(0x5B, 0x9C, 0xFF),
		GFX_RGB(0xB1, 0x8C, 0xFF), GFX_RGB(0xFF, 0x8C, 0xD9),
		GFX_RGB(0x4D, 0xD0, 0xC7), GFX_RGB(0xEC, 0xEE, 0xF2),
	};
	int x = w->x + PADDING, y = w->y + TITLEBAR_H + PADDING;
	int cell = (w->w - 2 * PADDING - 3 * 8) / 4;
	for (int i = 0; i < 8; i++) {
		int cx = x + (i % 4) * (cell + 8);
		int cy = y + (i / 4) * (cell + 8);
		gfx_fill_round_rect(cx, cy, cell, cell, 8, swatches[i]);
	}
}

static int create_window(int x, int y, int w, int h, const char *title, window_paint_fn paint, gfx_color_t accent) {
	for (int i = 0; i < MAX_WINDOWS; i++) {
		if (!windows[i].used) {
			windows[i].used = true;
			windows[i].x = x;
			windows[i].y = y;
			windows[i].w = w;
			windows[i].h = h;
			windows[i].paint = paint;
			windows[i].counter = 0;
			windows[i].accent = accent;
			strcpy(windows[i].title, title);
			window_order[window_count++] = i;
			return i;
		}
	}
	return -1;
}

void wm_init(void) {
	screen_w = gfx_width();
	screen_h = gfx_height();
	gfx_set_font_scale(2);

	memset(windows, 0, sizeof(windows));
	window_count = 0;

	int cx = screen_w / 2 - 340;
	int cy = screen_h / 2 - 230;
	create_window(cx, cy, 470, 250, "About auroraOS", paint_about, COL_ACCENT);
	create_window(cx + 510, cy, 280, 190, "Uptime", paint_counter, GFX_RGB(0x28, 0xC8, 0x40));
	create_window(cx + 100, cy + 290, 320, 160, "Palette", paint_palette, GFX_RGB(0xB1, 0x8C, 0xFF));
}

static void draw_titlebar_button(int x, int y, gfx_color_t color) {
	gfx_fill_round_rect(x, y, 12, 12, 6, color);
}

static void draw_window(struct window *w, bool active) {
	int total_h = w->h + TITLEBAR_H;

	gfx_draw_soft_shadow(w->x, w->y, w->w, total_h, CORNER_RADIUS, 14);

	gfx_color_t title_color = active ? COL_TITLE_ACT : COL_TITLE_INACT;

	/* body + titlebar as one rounded shape, body drawn square-topped
	 * underneath the rounded titlebar so the corners only round at top */
	gfx_fill_round_rect(w->x, w->y, w->w, total_h, CORNER_RADIUS, active ? COL_WIN_BODY : COL_WIN_BODY_ALT);
	gfx_fill_round_rect(w->x, w->y, w->w, TITLEBAR_H + CORNER_RADIUS, CORNER_RADIUS, title_color);
	gfx_fill_rect(w->x, w->y + TITLEBAR_H, w->w, CORNER_RADIUS, title_color);
	gfx_fill_rect(w->x, w->y + TITLEBAR_H, w->w, 1, GFX_RGB(0x00, 0x00, 0x00));

	/* traffic-light buttons */
	draw_titlebar_button(w->x + 14, w->y + TITLEBAR_H / 2 - 6, COL_CLOSE);
	draw_titlebar_button(w->x + 34, w->y + TITLEBAR_H / 2 - 6, COL_MIN);
	draw_titlebar_button(w->x + 54, w->y + TITLEBAR_H / 2 - 6, COL_MAX);

	gfx_color_t title_text = active ? COL_TEXT : COL_TEXT_DIM;
	int title_w = gfx_string_width(w->title);
	gfx_draw_string(w->x + (w->w - title_w) / 2, w->y + (TITLEBAR_H - gfx_char_height()) / 2, w->title, title_text);

	/* a thin accent strip under the titlebar when focused, a small
	 * modern touch instead of a hard border everywhere */
	if (active) {
		gfx_fill_rect(w->x + CORNER_RADIUS, w->y + total_h - 1, w->w - 2 * CORNER_RADIUS, 1, w->accent);
	}

	if (w->paint) w->paint(w);
}

static void draw_desktop_background(void) {
	/* vertical gradient - cheap but reads as "designed" rather than flat */
	for (int y = 0; y < screen_h - TASKBAR_H; y++) {
		int t = (y * 255) / (screen_h - TASKBAR_H);
		uint32_t r1 = (COL_DESKTOP_TOP >> 16) & 0xFF, g1 = (COL_DESKTOP_TOP >> 8) & 0xFF, b1 = COL_DESKTOP_TOP & 0xFF;
		uint32_t r2 = (COL_DESKTOP_BOTTOM >> 16) & 0xFF, g2 = (COL_DESKTOP_BOTTOM >> 8) & 0xFF, b2 = COL_DESKTOP_BOTTOM & 0xFF;
		uint32_t r = r1 + ((int)(r2 - r1) * t) / 255;
		uint32_t g = g1 + ((int)(g2 - g1) * t) / 255;
		uint32_t b = b1 + ((int)(b2 - b1) * t) / 255;
		gfx_draw_hline(0, y, screen_w, GFX_RGB(r, g, b));
	}
}

static void draw_taskbar(void) {
	int y = screen_h - TASKBAR_H;
	gfx_fill_rect(0, y, screen_w, TASKBAR_H, COL_TASKBAR);
	gfx_fill_rect(0, y, screen_w, 1, GFX_RGB(0x00, 0x00, 0x00));

	gfx_draw_string(20, y + (TASKBAR_H - gfx_char_height()) / 2, "auroraOS", COL_ACCENT);

	int bx = 150;
	int topmost = window_count > 0 ? window_order[window_count - 1] : -1;
	for (int i = 0; i < window_count; i++) {
		int idx = window_order[i];
		struct window *w = &windows[idx];
		if (!w->used) continue;
		int tw = gfx_string_width(w->title) + 32;
		bool active = (idx == topmost);
		gfx_fill_round_rect(bx, y + 8, tw, TASKBAR_H - 16, 8, active ? COL_TASKBAR_ACTIVE : GFX_RGB(0x1C, 0x1F, 0x2C));
		if (active) gfx_fill_rect(bx + 8, y + TASKBAR_H - 6, tw - 16, 2, w->accent);
		gfx_draw_string(bx + 16, y + (TASKBAR_H - gfx_char_height()) / 2, w->title, active ? COL_TEXT : COL_TEXT_DIM);
		bx += tw + 8;
	}

	/* clock-ish uptime readout on the right */
	char buf[16];
	uint32_t secs = timer_get_ticks() / 100;
	int i = 15;
	buf[i] = '\0';
	buf[--i] = 's';
	if (secs == 0) buf[--i] = '0';
	while (secs > 0 && i > 0) {
		buf[--i] = '0' + (secs % 10);
		secs /= 10;
	}
	int tw = gfx_string_width(&buf[i]);
	gfx_draw_string(screen_w - tw - 20, y + (TASKBAR_H - gfx_char_height()) / 2, &buf[i], COL_TEXT_DIM);
}

static void draw_cursor(int x, int y) {
	static const char *shape[12] = {
		"X...........",
		"XX..........",
		"X.X.........",
		"X..X........",
		"X...X.......",
		"X....X......",
		"X.....X.....",
		"X......X....",
		"X.......X...",
		"X....XXXXX..",
		"X..XX.......",
		"XXX.........",
	};
	for (int row = 0; row < 12; row++) {
		for (int col = 0; col < 13; col++) {
			if (shape[row][col] == 'X') {
				gfx_blend_pixel(x + col + 1, y + row + 1, COL_BLACK, 140);
			}
		}
	}
	for (int row = 0; row < 12; row++) {
		for (int col = 0; col < 13; col++) {
			if (shape[row][col] == 'X') {
				gfx_putpixel(x + col, y + row, COL_WHITE);
			}
		}
	}
	for (int row = 0; row < 11; row++) {
		for (int col = 0; col < 12; col++) {
			bool here = shape[row][col] == 'X';
			bool right = shape[row][col + 1] == 'X';
			bool down = shape[row + 1][col] == 'X';
			if (here && (!right || !down)) {
				gfx_putpixel(x + col, y + row, COL_BLACK);
			}
		}
	}
}

static void handle_click(int x, int y) {
	int idx = topmost_window_at(x, y);
	if (idx < 0) return;

	struct window *w = &windows[idx];
	raise_window(idx);

	if (y >= w->y + TITLEBAR_H / 2 - 6 && y < w->y + TITLEBAR_H / 2 + 6 &&
	    x >= w->x + 14 && x < w->x + 26) {
		w->used = false;
		for (int i = 0; i < window_count; i++) {
			if (window_order[i] == idx) {
				for (int j = i; j < window_count - 1; j++) {
					window_order[j] = window_order[j + 1];
				}
				window_count--;
				break;
			}
		}
		return;
	}

	if (y < w->y + TITLEBAR_H) {
		dragging_window = idx;
		drag_offset_x = x - w->x;
		drag_offset_y = y - w->y;
	}
}

void wm_run(void) {
	mouse_set_bounds(screen_w, screen_h);

	bool prev_button_down = false;

	for (;;) {
		int mx, my;
		uint8_t buttons;
		mouse_get_state(&mx, &my, &buttons);
		bool button_down = (buttons & 0x01) != 0;

		if (button_down && !prev_button_down) {
			handle_click(mx, my);
		} else if (!button_down) {
			dragging_window = -1;
		}

		if (dragging_window >= 0 && button_down) {
			struct window *w = &windows[dragging_window];
			w->x = mx - drag_offset_x;
			w->y = my - drag_offset_y;
			if (w->x < 0) w->x = 0;
			if (w->y < 0) w->y = 0;
			if (w->x + w->w > screen_w) w->x = screen_w - w->w;
			if (w->y + w->h + TITLEBAR_H > screen_h - TASKBAR_H)
				w->y = screen_h - TASKBAR_H - w->h - TITLEBAR_H;
		}

		prev_button_down = button_down;

		for (int i = 0; i < MAX_WINDOWS; i++) {
			if (windows[i].used) windows[i].counter = (int)timer_get_ticks();
		}

		draw_desktop_background();
		for (int i = 0; i < window_count; i++) {
			struct window *w = &windows[window_order[i]];
			if (w->used) draw_window(w, i == window_count - 1);
		}
		draw_taskbar();
		draw_cursor(mx, my);
		gfx_flip();

		timer_wait(1);

		if (keyboard_has_key()) {
			char c = keyboard_getchar_blocking();
			if (c == 'q') {
				gfx_set_font_scale(1);
				return;
			}
		}
	}
}
