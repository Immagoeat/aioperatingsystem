/* wm.c - a tiny windowing compositor: draggable windows with title bars,
 * a desktop background, a taskbar/clock, and a couple of demo apps. */
#include "kernel.h"

#define MAX_WINDOWS 8
#define TITLEBAR_H 12
#define TASKBAR_H 14

#define COL_DESKTOP     16
#define COL_WIN_BODY    17
#define COL_WIN_SHADOW  18
#define COL_TITLE_ACT   19
#define COL_TITLE_INACT 20
#define COL_BTN_FACE    21
#define COL_BTN_HI      22
#define COL_BTN_SHADOW  23
#define COL_WHITE       15
#define COL_BLACK       0
#define COL_TASKBAR     8
#define COL_TASKBAR_HI  7

struct window;
typedef void (*window_paint_fn)(struct window *w);

struct window {
	bool used;
	int x, y, w, h;
	char title[32];
	window_paint_fn paint;
	int app_id;
	int counter; /* generic per-app state */
};

static struct window windows[MAX_WINDOWS];
static int window_order[MAX_WINDOWS]; /* back-to-front */
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

/* --- Demo app: "About" window --- */
static void paint_about(struct window *w) {
	int x = w->x, y = w->y + TITLEBAR_H;
	gfx_draw_string(x + 8, y + 10, "auroraOS", COL_WHITE);
	gfx_draw_string(x + 8, y + 24, "graphical shell demo", COL_WHITE);
	gfx_draw_string(x + 8, y + 40, "Drag windows by their", COL_WHITE);
	gfx_draw_string(x + 8, y + 50, "title bar. Click the", COL_WHITE);
	gfx_draw_string(x + 8, y + 60, "taskbar icons to raise", COL_WHITE);
	gfx_draw_string(x + 8, y + 70, "a window.", COL_WHITE);
}

/* --- Demo app: counter/clock window --- */
static void paint_counter(struct window *w) {
	int x = w->x, y = w->y + TITLEBAR_H;
	gfx_draw_string(x + 8, y + 10, "Uptime ticks:", COL_WHITE);

	char buf[16];
	uint32_t val = (uint32_t)w->counter;
	int i = 15;
	buf[i] = '\0';
	if (val == 0) buf[--i] = '0';
	while (val > 0 && i > 0) {
		buf[--i] = '0' + (val % 10);
		val /= 10;
	}
	gfx_draw_string(x + 8, y + 24, &buf[i], COL_WHITE);

	/* simple animated bar */
	int barw = (w->counter / 2) % (w->w - 16);
	gfx_fill_rect(x + 8, y + 40, w->w - 16, 8, COL_BLACK);
	gfx_fill_rect(x + 8, y + 40, barw, 8, 11);
}

/* --- Demo app: palette swatch window --- */
static void paint_palette(struct window *w) {
	int x = w->x, y = w->y + TITLEBAR_H;
	int sw = (w->w - 16) / 8;
	for (int i = 0; i < 16; i++) {
		int cx = x + 8 + (i % 8) * sw;
		int cy = y + 10 + (i / 8) * sw;
		gfx_fill_rect(cx, cy, sw - 2, sw - 2, (uint8_t)i);
	}
}

static int create_window(int x, int y, int w, int h, const char *title, window_paint_fn paint) {
	for (int i = 0; i < MAX_WINDOWS; i++) {
		if (!windows[i].used) {
			windows[i].used = true;
			windows[i].x = x;
			windows[i].y = y;
			windows[i].w = w;
			windows[i].h = h;
			windows[i].paint = paint;
			windows[i].counter = 0;
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

	memset(windows, 0, sizeof(windows));
	window_count = 0;

	create_window(20, 20, 140, 90, "About", paint_about);
	create_window(180, 20, 110, 70, "Uptime", paint_counter);
	create_window(60, 90, 130, 50, "Palette", paint_palette);
}

static void draw_window(struct window *w, bool active) {
	int total_h = w->h + TITLEBAR_H;

	/* drop shadow */
	gfx_fill_rect(w->x + 3, w->y + total_h, w->w, 3, COL_WIN_SHADOW);
	gfx_fill_rect(w->x + w->w, w->y + 3, 3, total_h, COL_WIN_SHADOW);

	/* title bar */
	uint8_t tcolor = active ? COL_TITLE_ACT : COL_TITLE_INACT;
	gfx_fill_rect(w->x, w->y, w->w, TITLEBAR_H, tcolor);
	gfx_draw_string(w->x + 3, w->y + 2, w->title, COL_WHITE);

	/* close button */
	gfx_fill_rect(w->x + w->w - 10, w->y + 2, 8, 8, COL_BTN_FACE);
	gfx_draw_rect(w->x + w->w - 10, w->y + 2, 8, 8, COL_BLACK);
	gfx_draw_line(w->x + w->w - 9, w->y + 3, w->x + w->w - 3, w->y + 9, COL_BLACK);
	gfx_draw_line(w->x + w->w - 3, w->y + 3, w->x + w->w - 9, w->y + 9, COL_BLACK);

	/* body */
	gfx_fill_rect(w->x, w->y + TITLEBAR_H, w->w, w->h, COL_WIN_BODY);
	gfx_draw_rect(w->x, w->y, w->w, total_h, COL_BLACK);
	gfx_draw_hline(w->x + 1, w->y + TITLEBAR_H, w->w - 2, COL_BLACK);

	if (w->paint) w->paint(w);
}

static void draw_desktop_background(void) {
	gfx_fill_rect(0, 0, screen_w, screen_h - TASKBAR_H, COL_DESKTOP);
	/* a little decorative pattern so it doesn't look flat */
	for (int y = 0; y < screen_h - TASKBAR_H; y += 16) {
		gfx_draw_hline(0, y, screen_w, 17);
	}
}

static void draw_taskbar(void) {
	int y = screen_h - TASKBAR_H;
	gfx_fill_rect(0, y, screen_w, TASKBAR_H, COL_TASKBAR);
	gfx_draw_hline(0, y, screen_w, COL_TASKBAR_HI);
	gfx_draw_string(4, y + 3, "auroraOS", COL_WHITE);

	int bx = 70;
	for (int i = 0; i < window_count; i++) {
		struct window *w = &windows[window_order[i]];
		if (!w->used) continue;
		int bw = 60;
		gfx_fill_rect(bx, y + 2, bw, TASKBAR_H - 4, COL_BTN_FACE);
		gfx_draw_rect(bx, y + 2, bw, TASKBAR_H - 4, COL_BLACK);
		gfx_draw_string(bx + 3, y + 3, w->title, COL_BLACK);
		bx += bw + 4;
	}
}

static void draw_cursor(int x, int y) {
	static const char *cursor_shape[10] = {
		"X.........",
		"XX........",
		"X.X.......",
		"X..X......",
		"X...X.....",
		"X....X....",
		"X.....X...",
		"X......X..",
		"X.......X.",
		"X.XXXX....",
	};
	for (int row = 0; row < 10; row++) {
		for (int col = 0; col < 10; col++) {
			if (cursor_shape[row][col] == 'X') {
				gfx_putpixel(x + col, y + row, COL_BLACK);
			}
		}
	}
	for (int row = 0; row < 9; row++) {
		for (int col = 0; col < 9; col++) {
			if (cursor_shape[row][col] == 'X' &&
			    row > 0 && col > 0 &&
			    cursor_shape[row - 1][col - 1] != 'X') {
				gfx_putpixel(x + col - 1, y + row - 1, COL_WHITE);
			}
		}
	}
}

static void handle_click(int x, int y) {
	int idx = topmost_window_at(x, y);
	if (idx < 0) return;

	struct window *w = &windows[idx];
	raise_window(idx);

	/* close button hit test */
	if (y >= w->y + 2 && y < w->y + 10 &&
	    x >= w->x + w->w - 10 && x < w->x + w->w - 2) {
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

	/* start drag if clicked on title bar */
	if (y < w->y + TITLEBAR_H) {
		dragging_window = idx;
		drag_offset_x = x - w->x;
		drag_offset_y = y - w->y;
	}
}

void wm_run(void) {
	mouse_set_bounds(screen_w, screen_h);

	bool prev_button_down = false;
	uint32_t frame = 0;

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

		/* update per-app animated state */
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

		frame++;
		timer_wait(1); /* ~100Hz cap -> smooth enough, low CPU spin */

		/* Escape hatch: pressing 'q' on keyboard drops back to text shell */
		/* (checked non-blockingly via keyboard buffer below) */
		if (keyboard_has_key()) {
			char c = keyboard_getchar_blocking();
			if (c == 'q') {
				return;
			}
		}
	}
}
