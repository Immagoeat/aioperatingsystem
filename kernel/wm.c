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

	bool minimized;
	bool fullscreen;
	int restore_x, restore_y, restore_w, restore_h; /* geometry before fullscreen, for the maximize button to restore */
};

static struct window windows[MAX_WINDOWS];
static int window_order[MAX_WINDOWS];
static int window_count = 0;

static int dragging_window = -1;
static int drag_offset_x, drag_offset_y;

static int wallpaper_btn_x, wallpaper_btn_y, wallpaper_btn_w, wallpaper_btn_h;
static int search_btn_x, search_btn_y, search_btn_w, search_btn_h;

#define SEARCH_QUERY_MAX 32
static bool search_open = false;
static char search_query[SEARCH_QUERY_MAX] = "";
static int search_query_len = 0;
static int search_selected = 0; /* index into the filtered match list */
static int search_panel_x, search_panel_y, search_panel_w, search_panel_h;
static int search_row_y0; /* y of the first result row, for click hit-testing */

static int screen_w, screen_h;

/* Cached bounds of the taskbar's per-window buttons, recomputed each
 * frame in draw_taskbar() so handle_click() can hit-test them without
 * re-deriving the same layout logic twice. */
struct taskbar_button {
	int window_idx;
	int x, y, w, h;
};
static struct taskbar_button taskbar_buttons[MAX_WINDOWS];
static int taskbar_button_count = 0;

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
		if (!w->used || w->minimized) continue;
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
	gfx_draw_string(x, y, "title bar. The dots close,", COL_TEXT_DIM); y += lh;
	gfx_draw_string(x, y, "minimize, and fullscreen it.", COL_TEXT_DIM); y += lh;
	gfx_draw_string(x, y, "Click a taskbar icon to", COL_TEXT_DIM); y += lh;
	gfx_draw_string(x, y, "focus or restore a window.", COL_TEXT_DIM); y += lh;
	gfx_draw_string(x, y, "Press / to search for apps,", COL_TEXT_DIM); y += lh;
	gfx_draw_string(x, y, "Ctrl+Q for the shell.", COL_TEXT_DIM);
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
			windows[i].minimized = false;
			windows[i].fullscreen = false;
			strcpy(windows[i].title, title);
			window_order[window_count++] = i;
			return i;
		}
	}
	return -1;
}

/* --- app registry: default geometry/paint fn for each launchable app, so
 * the search bar can (re)open one by name even after it's been closed,
 * the same way a real OS's app launcher works rather than the search
 * only being able to find windows that already happen to be open. */
struct app_entry {
	const char *name;
	int x_offset, y_offset, w, h;
	window_paint_fn paint;
	gfx_color_t accent;
};

#define MAX_APPS 8
static struct app_entry apps[MAX_APPS];
static int app_count = 0;
static int base_cx, base_cy;

static void register_app(const char *name, int x_offset, int y_offset, int w, int h, window_paint_fn paint, gfx_color_t accent) {
	if (app_count >= MAX_APPS) return;
	apps[app_count++] = (struct app_entry){ name, x_offset, y_offset, w, h, paint, accent };
}

static int find_open_window_by_name(const char *name) {
	for (int i = 0; i < MAX_WINDOWS; i++) {
		if (windows[i].used && strcmp(windows[i].title, name) == 0) return i;
	}
	return -1;
}

static void launch_or_focus_app(int app_index) {
	if (app_index < 0 || app_index >= app_count) return;
	struct app_entry *app = &apps[app_index];

	int idx = find_open_window_by_name(app->name);
	if (idx < 0) {
		idx = create_window(base_cx + app->x_offset, base_cy + app->y_offset, app->w, app->h, app->name, app->paint, app->accent);
	}
	if (idx >= 0) {
		windows[idx].minimized = false;
		raise_window(idx);
	}
}

void wm_init(void) {
	screen_w = gfx_width();
	screen_h = gfx_height();
	gfx_set_font_scale(2);

	memset(windows, 0, sizeof(windows));
	window_count = 0;
	app_count = 0;

	base_cx = screen_w / 2 - 340;
	base_cy = screen_h / 2 - 230;

	register_app("About auroraOS", 0, 0, 500, 320, paint_about, COL_ACCENT);
	register_app("Uptime", 540, 0, 280, 190, paint_counter, GFX_RGB(0x28, 0xC8, 0x40));
	register_app("Palette", 100, 290, 320, 160, paint_palette, GFX_RGB(0xB1, 0x8C, 0xFF));

	for (int i = 0; i < app_count; i++) launch_or_focus_app(i);
}

static void draw_titlebar_button(int x, int y, gfx_color_t color) {
	gfx_fill_round_rect(x, y, 12, 12, 6, color);
}

static void toggle_fullscreen(struct window *w) {
	if (!w->fullscreen) {
		w->restore_x = w->x;
		w->restore_y = w->y;
		w->restore_w = w->w;
		w->restore_h = w->h;
		w->fullscreen = true;
		w->x = 0;
		w->y = 0;
		w->w = screen_w;
		w->h = screen_h - TASKBAR_H - TITLEBAR_H;
	} else {
		w->fullscreen = false;
		w->x = w->restore_x;
		w->y = w->restore_y;
		w->w = w->restore_w;
		w->h = w->restore_h;
	}
}

/* small pixel-art magnifying glass icon, centered at (cx, cy) */
static void draw_search_glyph(int cx, int cy, gfx_color_t color) {
	int ring_cx = cx - 2, ring_cy = cy - 2;
	int r_outer = 5, r_inner = 4;
	for (int dy = -r_outer; dy <= r_outer; dy++) {
		for (int dx = -r_outer; dx <= r_outer; dx++) {
			int d2 = dx * dx + dy * dy;
			if (d2 <= r_outer * r_outer && d2 >= r_inner * r_inner) {
				gfx_putpixel(ring_cx + dx, ring_cy + dy, color);
			}
		}
	}
	gfx_draw_line(cx + 2, cy + 2, cx + 6, cy + 6, color);
	gfx_draw_line(cx + 3, cy + 2, cx + 6, cy + 5, color);
}

/* case-insensitive substring test - the shared libc only has exact
 * strcmp/strncmp, and this is only needed here for search filtering */
static char lower_char(char c) {
	if (c >= 'A' && c <= 'Z') return (char)(c + 32);
	return c;
}

static bool contains_ci(const char *haystack, const char *needle) {
	if (!*needle) return true;
	for (const char *h = haystack; *h; h++) {
		const char *hh = h;
		const char *nn = needle;
		while (*hh && *nn && lower_char(*hh) == lower_char(*nn)) {
			hh++;
			nn++;
		}
		if (!*nn) return true;
	}
	return false;
}

/* Filters apps[] by the current search query, writing matching app
 * indices into out[] (capacity MAX_APPS). Returns the match count. */
static int search_matches(int *out) {
	int n = 0;
	for (int i = 0; i < app_count; i++) {
		if (contains_ci(apps[i].name, search_query)) {
			out[n++] = i;
		}
	}
	return n;
}

static void draw_window(struct window *w, bool active) {
	int total_h = w->h + TITLEBAR_H;
	/* fullscreen windows fill the desktop edge-to-edge: no floating
	 * shadow or rounded corners, the same way a real OS drops window
	 * chrome once a window fills the whole screen */
	int radius = w->fullscreen ? 0 : CORNER_RADIUS;

	if (!w->fullscreen) {
		gfx_draw_soft_shadow(w->x, w->y, w->w, total_h, radius, 14);
	}

	gfx_color_t title_color = active ? COL_TITLE_ACT : COL_TITLE_INACT;

	/* body + titlebar as one rounded shape, body drawn square-topped
	 * underneath the rounded titlebar so the corners only round at top */
	gfx_fill_round_rect(w->x, w->y, w->w, total_h, radius, active ? COL_WIN_BODY : COL_WIN_BODY_ALT);
	gfx_fill_round_rect(w->x, w->y, w->w, TITLEBAR_H + radius, radius, title_color);
	gfx_fill_rect(w->x, w->y + TITLEBAR_H, w->w, radius, title_color);
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
		gfx_fill_rect(w->x + radius, w->y + total_h - 1, w->w - 2 * radius, 1, w->accent);
	}

	if (w->paint) w->paint(w);
}

static int current_wallpaper = 0;

static void draw_gradient_fallback(void) {
	/* vertical gradient - used only if no baked-in wallpaper is available */
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

static void draw_desktop_background(void) {
	if (wallpaper_count() > 0) {
		wallpaper_draw(current_wallpaper, screen_w, screen_h);
	} else {
		draw_gradient_fallback();
	}
}

static void cycle_wallpaper(void) {
	int count = wallpaper_count();
	if (count <= 0) return;
	current_wallpaper = (current_wallpaper + 1) % count;
}

static void draw_taskbar(void) {
	int y = screen_h - TASKBAR_H;
	gfx_fill_rect(0, y, screen_w, TASKBAR_H, COL_TASKBAR);
	gfx_fill_rect(0, y, screen_w, 1, GFX_RGB(0x00, 0x00, 0x00));

	gfx_draw_string(20, y + (TASKBAR_H - gfx_char_height()) / 2, "auroraOS", COL_ACCENT);

	/* search button */
	search_btn_x = 150;
	search_btn_y = y + 8;
	search_btn_w = TASKBAR_H - 16;
	search_btn_h = TASKBAR_H - 16;
	gfx_fill_round_rect(search_btn_x, search_btn_y, search_btn_w, search_btn_h, 8,
		search_open ? COL_TASKBAR_ACTIVE : GFX_RGB(0x1C, 0x1F, 0x2C));
	draw_search_glyph(search_btn_x + search_btn_w / 2, search_btn_y + search_btn_h / 2, COL_TEXT);

	/* Compute the right-side widgets' widths first so the window-button
	 * row in the middle knows how much space it actually has and can
	 * stop (rather than overflow underneath them) before running out. */
	static char clock_buf[9] = "--:--:--";
	static uint32_t clock_last_tick = 0;
	uint32_t now = timer_get_ticks();
	if (now - clock_last_tick >= 100 || clock_last_tick == 0) {
		clock_last_tick = now;
		struct rtc_time t;
		rtc_get_time(&t);
		clock_buf[0] = '0' + (t.hours / 10);
		clock_buf[1] = '0' + (t.hours % 10);
		clock_buf[2] = ':';
		clock_buf[3] = '0' + (t.minutes / 10);
		clock_buf[4] = '0' + (t.minutes % 10);
		clock_buf[5] = ':';
		clock_buf[6] = '0' + (t.seconds / 10);
		clock_buf[7] = '0' + (t.seconds % 10);
		clock_buf[8] = '\0';
	}
	int clock_w = gfx_string_width(clock_buf);

	const struct wallpaper *wp = wallpaper_get(current_wallpaper);
	const char *wp_label = wp ? wp->name : "Wallpaper";
	int wp_btn_w = gfx_string_width(wp_label) + 40;
	wallpaper_btn_x = screen_w - clock_w - 40 - wp_btn_w;
	wallpaper_btn_y = y + 8;
	wallpaper_btn_w = wp_btn_w;
	wallpaper_btn_h = TASKBAR_H - 16;

	int bx = search_btn_x + search_btn_w + 12;
	int bx_max = wallpaper_btn_x - 12; /* don't draw window buttons under the right-side widgets */
	int topmost = -1;
	for (int i = window_count - 1; i >= 0; i--) {
		int idx = window_order[i];
		if (windows[idx].used && !windows[idx].minimized) { topmost = idx; break; }
	}
	taskbar_button_count = 0;
	for (int i = 0; i < window_count; i++) {
		int idx = window_order[i];
		struct window *w = &windows[idx];
		if (!w->used) continue;
		int tw = gfx_string_width(w->title) + 32;
		if (bx + tw > bx_max) break; /* out of room; skip remaining buttons */
		bool active = (idx == topmost);
		gfx_color_t text_color = w->minimized ? COL_TEXT_DIM : (active ? COL_TEXT : COL_TEXT_DIM);
		gfx_fill_round_rect(bx, y + 8, tw, TASKBAR_H - 16, 8,
			active ? COL_TASKBAR_ACTIVE : (w->minimized ? GFX_RGB(0x15, 0x17, 0x22) : GFX_RGB(0x1C, 0x1F, 0x2C)));
		if (active) gfx_fill_rect(bx + 8, y + TASKBAR_H - 6, tw - 16, 2, w->accent);
		if (w->minimized) {
			/* small dash icon hints "this is minimized, click to restore" */
			gfx_fill_rect(bx + 8, y + TASKBAR_H / 2 - 1, 8, 2, COL_TEXT_DIM);
			gfx_draw_string(bx + 22, y + (TASKBAR_H - gfx_char_height()) / 2, w->title, text_color);
		} else {
			gfx_draw_string(bx + 16, y + (TASKBAR_H - gfx_char_height()) / 2, w->title, text_color);
		}

		if (taskbar_button_count < MAX_WINDOWS) {
			taskbar_buttons[taskbar_button_count++] = (struct taskbar_button){ idx, bx, y + 8, tw, TASKBAR_H - 16 };
		}

		bx += tw + 8;
	}

	/* real wall-clock readout, on the far right */
	gfx_draw_string(screen_w - clock_w - 20, y + (TASKBAR_H - gfx_char_height()) / 2, clock_buf, COL_TEXT_DIM);

	/* wallpaper-swap button, just left of the clock */
	gfx_fill_round_rect(wallpaper_btn_x, wallpaper_btn_y, wallpaper_btn_w, wallpaper_btn_h, 8, GFX_RGB(0x1C, 0x1F, 0x2C));
	gfx_draw_string(wallpaper_btn_x + 12, y + (TASKBAR_H - gfx_char_height()) / 2, wp_label, COL_TEXT_DIM);
	/* small swatch icon to hint "click to change" */
	gfx_fill_round_rect(wallpaper_btn_x + wp_btn_w - 26, y + TASKBAR_H / 2 - 6, 12, 12, 4, COL_ACCENT);
}

#define SEARCH_PANEL_W 320
#define SEARCH_ROW_H 36

static void draw_search_panel(void) {
	if (!search_open) return;

	int panel_x = search_btn_x;
	int panel_y = screen_h - TASKBAR_H - 12;
	int matches[MAX_APPS];
	int match_count = search_matches(matches);
	int panel_h = 56 + match_count * SEARCH_ROW_H + (match_count == 0 ? SEARCH_ROW_H : 0);
	panel_y -= panel_h - 56; /* grow upward from the taskbar */

	search_panel_x = panel_x;
	search_panel_y = panel_y;
	search_panel_w = SEARCH_PANEL_W;
	search_panel_h = panel_h;
	search_row_y0 = panel_y + 56;

	gfx_draw_soft_shadow(panel_x, panel_y, SEARCH_PANEL_W, panel_h, CORNER_RADIUS, 12);
	gfx_fill_round_rect(panel_x, panel_y, SEARCH_PANEL_W, panel_h, CORNER_RADIUS, COL_WIN_BODY);
	gfx_draw_rect(panel_x, panel_y, SEARCH_PANEL_W, panel_h, GFX_RGB(0x00, 0x00, 0x00));

	/* input field */
	int field_x = panel_x + 12, field_y = panel_y + 12;
	int field_w = SEARCH_PANEL_W - 24, field_h = 32;
	gfx_fill_round_rect(field_x, field_y, field_w, field_h, 6, COL_WIN_BODY_ALT);
	draw_search_glyph(field_x + 16, field_y + field_h / 2, COL_TEXT_DIM);

	int text_x = field_x + 34;
	if (search_query_len > 0) {
		gfx_draw_string(text_x, field_y + (field_h - gfx_char_height()) / 2, search_query, COL_TEXT);
	} else {
		gfx_draw_string(text_x, field_y + (field_h - gfx_char_height()) / 2, "Search apps...", COL_TEXT_DIM);
	}
	/* blinking cursor at end of typed text */
	if (((timer_get_ticks() / 30) % 2) == 0) {
		int cursor_x = text_x + gfx_string_width(search_query);
		gfx_fill_rect(cursor_x + 2, field_y + 6, 2, field_h - 12, COL_ACCENT);
	}

	/* results list */
	int row_y = panel_y + 56;
	if (match_count == 0) {
		gfx_draw_string(field_x, row_y + (SEARCH_ROW_H - gfx_char_height()) / 2, "No matching apps", COL_TEXT_DIM);
	}
	for (int i = 0; i < match_count; i++) {
		bool selected = (i == search_selected);
		if (selected) {
			gfx_fill_round_rect(field_x, row_y, field_w, SEARCH_ROW_H - 4, 6, COL_TASKBAR_ACTIVE);
		}
		struct app_entry *app = &apps[matches[i]];
		gfx_fill_round_rect(field_x + 8, row_y + (SEARCH_ROW_H - 4 - 14) / 2, 14, 14, 4, app->accent);
		gfx_draw_string(field_x + 32, row_y + (SEARCH_ROW_H - 4 - gfx_char_height()) / 2, app->name, selected ? COL_TEXT : COL_TEXT_DIM);
		row_y += SEARCH_ROW_H;
	}
}

/* A detailed, smooth-edged cursor modeled on a real desktop OS arrow:
 * a tapered silhouette (not a blocky diagonal staircase) at native pixel
 * resolution with soft anti-aliased edges, a crisp dark outline, a subtle
 * highlight down the spine for a bit of dimensionality, and a soft drop
 * shadow - so it reads clearly over both light and dark wallpapers
 * without looking like flat pixel art. */
#define CURSOR_ROWS 24
#define CURSOR_COLS 18

/* 'X' = solid fill, 'B' = solid outline, 'H' = highlight, '.' = empty.
 * A soft (partially transparent) anti-aliasing pass runs after this,
 * so the shape only needs to define the crisp core. */
static const char *cursor_shape[CURSOR_ROWS] = {
	"BB................",
	"BXB...............",
	"BXXB..............",
	"BHXXB.............",
	"BHXXXB............",
	"BHXXXXB...........",
	"BHXXXXXB..........",
	"BHXXXXXXB.........",
	"BHXXXXXXXB........",
	"BHXXXXXXXXB.......",
	"BHXXXXXXXXXB......",
	"BHXXXXXXXXXXB.....",
	"BHXXXXXXBBBBB.....",
	"BHXXXHXXB.........",
	"BHXXXXXXB.........",
	"BHXXBHXXXB........",
	"BHXXB.HXXXB.......",
	"BHXB..HXXXB.......",
	"BXB....BXXXB......",
	"BB.....BXXXB......",
	"B.......BXXXB.....",
	".........BXXXB....",
	".........BBBBB....",
	"..................",
};

static gfx_color_t cursor_cell_color(char c) {
	switch (c) {
		case 'X': return COL_WHITE;
		case 'B': return COL_BLACK;
		case 'H': return GFX_RGB(0xC8, 0xD8, 0xF0); /* faint cool highlight down the spine */
		default: return 0;
	}
}

static void draw_cursor(int x, int y) {
	/* soft drop shadow: blur the whole silhouette outward a couple of
	 * pixels at low alpha, offset down-right, for a sense of depth
	 * rather than the cursor looking pasted flat onto the desktop */
	for (int row = 0; row < CURSOR_ROWS; row++) {
		for (int col = 0; col < CURSOR_COLS; col++) {
			if (cursor_shape[row][col] == '.') continue;
			for (int sd = 2; sd >= 1; sd--) {
				uint8_t alpha = sd == 1 ? 70 : 35;
				gfx_blend_pixel(x + col + sd, y + row + sd, COL_BLACK, alpha);
			}
		}
	}

	/* anti-aliased edge: any empty cell adjacent to a filled one gets a
	 * half-strength blend of the outline color, softening the silhouette
	 * boundary instead of leaving a hard aliased pixel edge */
	static const int dr[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
	static const int dc[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
	for (int row = 0; row < CURSOR_ROWS; row++) {
		for (int col = 0; col < CURSOR_COLS; col++) {
			if (cursor_shape[row][col] != '.') continue;
			bool touches_shape = false;
			for (int d = 0; d < 8; d++) {
				int nr = row + dr[d], nc = col + dc[d];
				if (nr >= 0 && nr < CURSOR_ROWS && nc >= 0 && nc < CURSOR_COLS &&
				    cursor_shape[nr][nc] != '.') {
					touches_shape = true;
					break;
				}
			}
			if (touches_shape) {
				gfx_blend_pixel(x + col, y + row, COL_BLACK, 90);
			}
		}
	}

	/* crisp core on top */
	for (int row = 0; row < CURSOR_ROWS; row++) {
		for (int col = 0; col < CURSOR_COLS; col++) {
			char c = cursor_shape[row][col];
			if (c == '.') continue;
			gfx_putpixel(x + col, y + row, cursor_cell_color(c));
		}
	}
}

static bool point_in(int x, int y, int rx, int ry, int rw, int rh) {
	return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void open_search(void) {
	search_open = true;
	search_query[0] = '\0';
	search_query_len = 0;
	search_selected = 0;
}

static void close_search(void) {
	search_open = false;
}

static void launch_selected_search_result(void) {
	int matches[MAX_APPS];
	int match_count = search_matches(matches);
	if (match_count == 0) return;
	if (search_selected >= match_count) search_selected = match_count - 1;
	launch_or_focus_app(matches[search_selected]);
	close_search();
}

static void handle_click(int x, int y) {
	if (point_in(x, y, search_btn_x, search_btn_y, search_btn_w, search_btn_h)) {
		if (search_open) close_search(); else open_search();
		return;
	}

	if (search_open) {
		if (point_in(x, y, search_panel_x, search_panel_y, search_panel_w, search_panel_h)) {
			if (y >= search_row_y0) {
				int row = (y - search_row_y0) / SEARCH_ROW_H;
				int matches[MAX_APPS];
				int match_count = search_matches(matches);
				if (row >= 0 && row < match_count) {
					search_selected = row;
					launch_selected_search_result();
				}
			}
			return;
		}
		/* clicked outside the open search panel/button: close it, but
		 * still let the click reach whatever's underneath (e.g. a window) */
		close_search();
	}

	if (point_in(x, y, wallpaper_btn_x, wallpaper_btn_y, wallpaper_btn_w, wallpaper_btn_h)) {
		cycle_wallpaper();
		return;
	}

	for (int i = 0; i < taskbar_button_count; i++) {
		struct taskbar_button *btn = &taskbar_buttons[i];
		if (point_in(x, y, btn->x, btn->y, btn->w, btn->h)) {
			struct window *w = &windows[btn->window_idx];

			int topmost_visible = -1;
			for (int j = window_count - 1; j >= 0; j--) {
				int oidx = window_order[j];
				if (windows[oidx].used && !windows[oidx].minimized) { topmost_visible = oidx; break; }
			}

			if (w->minimized) {
				w->minimized = false;
				raise_window(btn->window_idx);
			} else if (btn->window_idx == topmost_visible) {
				w->minimized = true;
			} else {
				raise_window(btn->window_idx);
			}
			return;
		}
	}

	int idx = topmost_window_at(x, y);
	if (idx < 0) return;

	struct window *w = &windows[idx];
	raise_window(idx);

	bool in_button_row = y >= w->y + TITLEBAR_H / 2 - 6 && y < w->y + TITLEBAR_H / 2 + 6;

	if (in_button_row && x >= w->x + 14 && x < w->x + 26) {
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

	if (in_button_row && x >= w->x + 34 && x < w->x + 46) {
		w->minimized = true;
		return;
	}

	if (in_button_row && x >= w->x + 54 && x < w->x + 66) {
		toggle_fullscreen(w);
		return;
	}

	if (y < w->y + TITLEBAR_H) {
		if (w->fullscreen) return; /* fullscreen windows aren't draggable */
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

		int topmost_visible = -1;
		for (int i = window_count - 1; i >= 0; i--) {
			struct window *w = &windows[window_order[i]];
			if (w->used && !w->minimized) { topmost_visible = window_order[i]; break; }
		}

		draw_desktop_background();
		for (int i = 0; i < window_count; i++) {
			struct window *w = &windows[window_order[i]];
			if (w->used && !w->minimized) draw_window(w, window_order[i] == topmost_visible);
		}
		draw_taskbar();
		draw_search_panel();
		draw_cursor(mx, my);
		gfx_flip();

		timer_wait(1);

		if (keyboard_has_key()) {
			char c = keyboard_getchar_blocking();

			if (search_open) {
				if (c == 27) { /* Escape */
					close_search();
				} else if (c == '\n') {
					launch_selected_search_result();
				} else if (c == '\b') {
					if (search_query_len > 0) {
						search_query[--search_query_len] = '\0';
						search_selected = 0;
					}
				} else if (c >= 32 && c < 127 && search_query_len < SEARCH_QUERY_MAX - 1) {
					search_query[search_query_len++] = c;
					search_query[search_query_len] = '\0';
					search_selected = 0;
				}
			} else if (c == CTRL_KEY('q')) {
				gfx_set_font_scale(1);
				return;
			} else if (c == CTRL_KEY('w')) {
				cycle_wallpaper();
			} else if (c == '/') {
				open_search();
			}
		}
	}
}
