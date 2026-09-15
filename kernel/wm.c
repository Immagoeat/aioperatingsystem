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

/* Small procedural vector icons (drawn with the same gfx_fill_rect/
 * gfx_draw_line primitives every app already uses), one per launchable
 * app, replacing the old flat accent-color swatch in the search list.
 * Genuinely drawn shapes rather than embedded bitmap art - there's no
 * real icon artwork available to embed here (see wallpaper.c/
 * img_to_c.py for how actual bitmap assets get embedded when they do
 * exist), and a simple recognizable glyph per app is the honest
 * equivalent for a from-scratch OS with no icon theme of its own. */
enum app_icon {
	ICON_NONE = 0,
	ICON_INFO,      /* About */
	ICON_CLOCK,     /* Uptime */
	ICON_PALETTE,   /* Palette */
	ICON_GEAR,      /* Settings */
	ICON_DOCUMENT,  /* Text Editor */
	ICON_GLOBE,     /* Browser */
	ICON_TERMINAL,  /* Terminal */
};

struct window;
typedef void (*window_paint_fn)(struct window *w);
/* Per-window keyboard input, for apps that need more than a click (the
 * Settings picker's arrow keys, the Text Editor's actual typing). Only
 * the topmost non-minimized window ever receives keys this way - see
 * wm_run()'s input dispatch - and only apps that register one of these
 * (via register_interactive_app()) get keyboard focus at all; plain
 * paint-only apps like Palette are never sent keys. */
typedef void (*window_key_fn)(struct window *w, char c);

/* Per-window mouse-click input, for apps that need to react to a
 * click within their own content area (Settings' dropdowns) rather
 * than just keyboard input. `x`/`y` are already window-relative
 * (click position minus w->x/w->y), so a click handler never needs to
 * know its own window's screen position. Like window_key_fn, only the
 * topmost non-minimized window ever receives one, and only after the
 * titlebar buttons/drag handling above it in handle_click() have all
 * had a chance to claim the click first. */
typedef void (*window_click_fn)(struct window *w, int x, int y);

/* Computes an app's minimum content size from its actual text/layout
 * at the current font scale (gfx_char_width()/height()), instead of a
 * fixed pixel constant tuned by eye for one scale - the same window
 * that comfortably fits its text at one UI scale would either clip it
 * or waste space at another without this. Called once when a window
 * is (re)opened and again whenever the UI scale changes (see
 * rescale_all_windows()), not every frame - none of these apps' fixed
 * chrome text changes on its own between those points. */
typedef void (*window_natural_size_fn)(int *out_w, int *out_h);

struct window {
	bool used;
	int x, y, w, h;
	char title[32];
	window_paint_fn paint;
	window_key_fn key; /* NULL for apps with no keyboard interaction */
	window_click_fn click; /* NULL for apps with no content-area click interaction */
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
static int network_btn_x, network_btn_y, network_btn_w, network_btn_h;
static bool network_panel_open = false;
static int connect_btn_x, connect_btn_y, connect_btn_w, connect_btn_h;
static bool connect_attempted = false; /* true once a lease attempt has been made this session, success or failure */
static bool connecting_in_progress = false;

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
/* One source of truth for both drawing and sizing (see
 * about_natural_size() below) - a fixed pixel window tuned by eye for
 * one font scale would either clip this text or waste space at any
 * other scale, so the window's size is derived from these same
 * strings via gfx_string_width()/gfx_char_height() instead of a
 * separately-guessed constant. line[0] is drawn in the accent color
 * with extra spacing below (the "title"); the rest are dimmed body
 * text, with one extra blank-line's worth of gap after line[2]
 * (matching the paragraph break the old hardcoded version had). */
enum about_style { ABOUT_TITLE, ABOUT_BODY, ABOUT_DIM };
struct about_line { const char *text; enum about_style style; bool extra_gap_after; };
static const struct about_line about_lines[] = {
	{ "auroraOS", ABOUT_TITLE, false },
	{ "A tiny hobby kernel with a", ABOUT_BODY, false },
	{ "real graphical desktop.", ABOUT_BODY, true },
	{ "Drag windows by their", ABOUT_DIM, false },
	{ "title bar. The dots close,", ABOUT_DIM, false },
	{ "minimize, and fullscreen it.", ABOUT_DIM, false },
	{ "Click a taskbar icon to", ABOUT_DIM, false },
	{ "focus or restore a window.", ABOUT_DIM, false },
	{ "Press the Windows key to", ABOUT_DIM, false },
	{ "search for apps, Ctrl+Q for", ABOUT_DIM, false },
	{ "the shell.", ABOUT_DIM, false },
};
#define ABOUT_LINE_COUNT (int)(sizeof(about_lines) / sizeof(about_lines[0]))

static void about_natural_size(int *out_w, int *out_h) {
	int lh = gfx_char_height() + 6;
	int max_w = 0, total_h = 0;
	for (int i = 0; i < ABOUT_LINE_COUNT; i++) {
		int sw = gfx_string_width(about_lines[i].text);
		if (sw > max_w) max_w = sw;
		total_h += lh + (about_lines[i].style == ABOUT_TITLE ? 4 : 0) + (about_lines[i].extra_gap_after ? 8 : 0);
	}
	*out_w = max_w + 2 * PADDING;
	*out_h = total_h + 2 * PADDING;
}

static void paint_about(struct window *w) {
	int x = w->x + PADDING, y = w->y + TITLEBAR_H + PADDING;
	int lh = gfx_char_height() + 6;
	for (int i = 0; i < ABOUT_LINE_COUNT; i++) {
		const struct about_line *l = &about_lines[i];
		gfx_color_t color = l->style == ABOUT_TITLE ? w->accent : (l->style == ABOUT_BODY ? COL_TEXT : COL_TEXT_DIM);
		gfx_draw_string(x, y, l->text, color);
		y += lh + (l->style == ABOUT_TITLE ? 4 : 0) + (l->extra_gap_after ? 8 : 0);
	}
}

/* Uptime's actual content is short and fixed ("System uptime" plus a
 * counter that only grows a few digits over any realistic session),
 * so the real sizing constraint is leaving enough room for the
 * progress bar to read as a bar rather than a sliver - expressed here
 * in character-cell widths so it scales with the font the same way
 * the text does, rather than a flat pixel guess. */
static void counter_natural_size(int *out_w, int *out_h) {
	int cw = gfx_char_width(), ch = gfx_char_height();
	int text_w = gfx_string_width("System uptime");
	int bar_w = 22 * cw; /* wide enough to read as a progress bar */
	*out_w = (text_w > bar_w ? text_w : bar_w) + 2 * PADDING;
	*out_h = (ch + 6) * 2 + 12 + 10 + 2 * PADDING;
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

/* Palette has no text to clip (it's just colored swatches that
 * already scale to fill whatever width the window has), so its
 * "natural size" is really just a comfortable minimum swatch size,
 * expressed in character-cell units so it still grows/shrinks
 * proportionally with the font scale like every other window here. */
static void palette_natural_size(int *out_w, int *out_h) {
	int cell = gfx_char_height() * 2; /* comfortable swatch size relative to the current font */
	*out_w = 4 * cell + 3 * 8 + 2 * PADDING;
	*out_h = 2 * cell + 8 + 2 * PADDING;
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

/* --- Settings: a real window, like Palette, rather than a console
 * takeover - see settings.c for the timezone/UI-scale data this
 * draws. Two dropdowns (Timezone, UI Scale), each a closed field
 * showing the current selection that opens into a scrollable overlay
 * list on click - the standard dropdown pattern already used
 * elsewhere in this WM for the search panel and network panel (see
 * handle_click()), just attached to one specific window's content
 * area via window_click_fn instead of the desktop chrome. Per-window
 * state, indexed by window slot since only one Settings window can
 * exist at a time (same one-per-name rule every app here follows). */
static void rescale_all_windows(void); /* defined below, after the app registry it needs to read - forward-declared since Settings (above that point) is what triggers it */

enum settings_dropdown { SETTINGS_DROPDOWN_NONE = 0, SETTINGS_DROPDOWN_TIMEZONE, SETTINGS_DROPDOWN_SCALE };
static int settings_selected[MAX_WINDOWS];       /* timezone index */
static int settings_scale_selected[MAX_WINDOWS]; /* ui_scale index */
static bool settings_applied[MAX_WINDOWS];       /* true once a change has actually been applied this session, to show a confirmation */
static enum settings_dropdown settings_open[MAX_WINDOWS];
static int settings_dropdown_scroll[MAX_WINDOWS]; /* first visible row of whichever dropdown is open */

/* Layout constants shared between paint_settings() (which draws at
 * these positions) and key_settings()/click_settings() (which need
 * the same positions to hit-test against) - kept as one set of
 * functions computing them from the window/font metrics so the two
 * can never drift out of sync with each other. */
/* All fields here are window-relative (relative to the window's own
 * top-left corner, i.e. what you'd add w->x/w->y to for an actual
 * screen position) - both paint_settings() (which adds w->x/w->y
 * before every gfx_* call) and click_settings() (which receives
 * already window-relative coordinates from handle_click()'s
 * dispatch - see window_click_fn's doc comment) work in this same
 * space, so a click can never land on the wrong field due to one of
 * them silently using absolute screen coordinates while the other
 * uses window-relative ones. */
struct settings_layout {
	int x, field_w, field_h;
	int tz_field_y;
	int scale_field_y;
	int lh;
};

static struct settings_layout settings_compute_layout(struct window *w) {
	struct settings_layout L;
	L.x = PADDING;
	L.field_w = w->w - 2 * PADDING;
	L.lh = gfx_char_height() + 4;
	L.field_h = gfx_char_height() + 14;

	int y = TITLEBAR_H + PADDING;
	y += L.lh + 6; /* "Timezone" section title */
	y += L.lh * 3 + 10; /* the three description lines below it */
	L.tz_field_y = y;
	y += L.field_h + 20;
	y += L.lh + 6; /* "UI Scale" section title */
	y += L.lh * 2 + 10; /* its two description lines */
	L.scale_field_y = y;

	return L;
}

/* Draws one closed dropdown field - shared between the Timezone and
 * UI Scale dropdowns since they're otherwise identical UI, just
 * backed by different option lists. The open overlay list itself is
 * a separate draw (draw_dropdown_overlay() below), issued afterwards
 * so it floats on top of whatever's below it rather than pushing
 * later content down - the same layering the search/network panels
 * already use. */
static void draw_dropdown_field(int x, int field_y, int field_w, int field_h,
                                 const char *current_label, bool is_open) {
	gfx_fill_round_rect(x, field_y, field_w, field_h, 6, is_open ? COL_WIN_BODY_ALT : COL_TASKBAR);
	gfx_draw_string(x + 10, field_y + (field_h - gfx_char_height()) / 2, current_label, COL_TEXT);

	/* small chevron on the right edge, pointing down when closed / up
	 * when open, so there's a visible affordance that this is
	 * expandable rather than just a label */
	int chev_x = x + field_w - 20, chev_y = field_y + field_h / 2;
	if (is_open) {
		gfx_draw_line(chev_x - 4, chev_y + 2, chev_x, chev_y - 3, COL_TEXT_DIM);
		gfx_draw_line(chev_x, chev_y - 3, chev_x + 4, chev_y + 2, COL_TEXT_DIM);
	} else {
		gfx_draw_line(chev_x - 4, chev_y - 2, chev_x, chev_y + 3, COL_TEXT_DIM);
		gfx_draw_line(chev_x, chev_y + 3, chev_x + 4, chev_y - 2, COL_TEXT_DIM);
	}
}

#define SETTINGS_DROPDOWN_VISIBLE_ROWS 6

static void draw_dropdown_overlay(int x, int field_y, int field_w, int field_h, int row_h,
                                   int count, int selected, int scroll,
                                   const char *(*label_at)(int)) {
	int visible = count < SETTINGS_DROPDOWN_VISIBLE_ROWS ? count : SETTINGS_DROPDOWN_VISIBLE_ROWS;
	int overlay_y = field_y + field_h + 4;
	int overlay_h = visible * row_h + 8;

	gfx_draw_soft_shadow(x, overlay_y, field_w, overlay_h, 8, 12);
	gfx_fill_round_rect(x, overlay_y, field_w, overlay_h, 8, COL_TITLE_ACT);

	for (int i = 0; i < visible; i++) {
		int opt_index = scroll + i;
		if (opt_index >= count) break;
		int row_y = overlay_y + 4 + i * row_h;
		bool is_selected = (opt_index == selected);
		if (is_selected) gfx_fill_round_rect(x + 4, row_y, field_w - 8, row_h - 2, 5, COL_TASKBAR_ACTIVE);
		gfx_draw_string(x + 12, row_y + (row_h - gfx_char_height()) / 2 - 1, label_at(opt_index), is_selected ? COL_TEXT : COL_TEXT_DIM);
	}
}

static const char *timezone_label_at(int i) {
	const struct timezone_option *opt = timezone_option_get(i);
	return opt ? opt->label : "";
}
static const char *ui_scale_label_at(int i) {
	const struct ui_scale_option *opt = ui_scale_option_get(i);
	return opt ? opt->label : "";
}

static void paint_settings(struct window *w) {
	int idx = (int)(w - windows);
	struct settings_layout L = settings_compute_layout(w);
	/* L.* is window-relative (see settings_compute_layout()'s comment);
	 * every gfx_* call below needs real screen coordinates, so w->x/
	 * w->y are added right here, once, rather than scattered through
	 * every draw call. */
	int x = w->x + L.x, y = w->y + TITLEBAR_H + PADDING;

	gfx_draw_string(x, y, "Timezone", w->accent); y += L.lh + 6;
	gfx_draw_string(x, y, "auroraOS can't detect your region (no", COL_TEXT_DIM); y += L.lh;
	gfx_draw_string(x, y, "network/GPS) - it assumes the clock is", COL_TEXT_DIM); y += L.lh;
	gfx_draw_string(x, y, "already local time. Pick a UTC offset:", COL_TEXT_DIM); y += L.lh + 10;

	draw_dropdown_field(x, w->y + L.tz_field_y, L.field_w, L.field_h, timezone_label_at(settings_selected[idx]), settings_open[idx] == SETTINGS_DROPDOWN_TIMEZONE);
	y = w->y + L.tz_field_y + L.field_h + 20;

	gfx_draw_string(x, y, "UI Scale", w->accent); y += L.lh + 6;
	gfx_draw_string(x, y, "There's no real video-mode switch here,", COL_TEXT_DIM); y += L.lh;
	gfx_draw_string(x, y, "so this resizes the desktop's UI instead:", COL_TEXT_DIM); y += L.lh + 10;

	draw_dropdown_field(x, w->y + L.scale_field_y, L.field_w, L.field_h, ui_scale_label_at(settings_scale_selected[idx]), settings_open[idx] == SETTINGS_DROPDOWN_SCALE);

	int footer_y = w->y + w->h + TITLEBAR_H - 24;
	if (settings_applied[idx]) {
		gfx_draw_string(x, footer_y, "Applied.", GFX_RGB(0x28, 0xC8, 0x40));
	} else {
		gfx_draw_string(x, footer_y, "Click a field to choose", COL_TEXT_DIM);
	}

	/* the open dropdown's overlay is drawn last so it floats over
	 * everything below it (the other field, the footer) rather than
	 * being drawn-under and looking clipped */
	if (settings_open[idx] == SETTINGS_DROPDOWN_TIMEZONE) {
		draw_dropdown_overlay(x, w->y + L.tz_field_y, L.field_w, L.field_h, L.lh + 4,
			timezone_option_count(), settings_selected[idx], settings_dropdown_scroll[idx], timezone_label_at);
	} else if (settings_open[idx] == SETTINGS_DROPDOWN_SCALE) {
		draw_dropdown_overlay(x, w->y + L.scale_field_y, L.field_w, L.field_h, L.lh + 4,
			ui_scale_option_count(), settings_scale_selected[idx], settings_dropdown_scroll[idx], ui_scale_label_at);
	}
}

static void settings_apply_scale(int idx) {
	const struct ui_scale_option *opt = ui_scale_option_get(settings_scale_selected[idx]);
	if (!opt) return;
	gfx_set_font_scale(opt->scale);
	rescale_all_windows();
	settings_applied[idx] = true;
}

static void key_settings(struct window *w, char c) {
	int idx = (int)(w - windows);

	if (settings_open[idx] == SETTINGS_DROPDOWN_NONE) {
		if (c == '\n') {
			settings_open[idx] = SETTINGS_DROPDOWN_TIMEZONE;
			settings_dropdown_scroll[idx] = settings_selected[idx];
		}
		return;
	}

	int count = settings_open[idx] == SETTINGS_DROPDOWN_TIMEZONE ? timezone_option_count() : ui_scale_option_count();
	int *selected = settings_open[idx] == SETTINGS_DROPDOWN_TIMEZONE ? &settings_selected[idx] : &settings_scale_selected[idx];

	if (c == KEY_ARROW_UP) {
		if (*selected > 0) (*selected)--;
		if (*selected < settings_dropdown_scroll[idx]) settings_dropdown_scroll[idx] = *selected;
	} else if (c == KEY_ARROW_DOWN) {
		if (*selected < count - 1) (*selected)++;
		if (*selected >= settings_dropdown_scroll[idx] + SETTINGS_DROPDOWN_VISIBLE_ROWS) settings_dropdown_scroll[idx] = *selected - SETTINGS_DROPDOWN_VISIBLE_ROWS + 1;
	} else if (c == 27) { /* Escape: close without changing anything further */
		settings_open[idx] = SETTINGS_DROPDOWN_NONE;
	} else if (c == '\n') {
		if (settings_open[idx] == SETTINGS_DROPDOWN_TIMEZONE) {
			const struct timezone_option *opt = timezone_option_get(settings_selected[idx]);
			if (opt) { rtc_set_timezone_offset_minutes(opt->offset_minutes); settings_applied[idx] = true; }
		} else {
			settings_apply_scale(idx);
		}
		settings_open[idx] = SETTINGS_DROPDOWN_NONE;
	}
}

static void click_settings(struct window *w, int x, int y) {
	int idx = (int)(w - windows);
	struct settings_layout L = settings_compute_layout(w);

	if (settings_open[idx] != SETTINGS_DROPDOWN_NONE) {
		int field_y = settings_open[idx] == SETTINGS_DROPDOWN_TIMEZONE ? L.tz_field_y : L.scale_field_y;
		int count = settings_open[idx] == SETTINGS_DROPDOWN_TIMEZONE ? timezone_option_count() : ui_scale_option_count();
		int row_h = L.lh + 4;
		int overlay_y = field_y + L.field_h + 4;
		int visible = count < SETTINGS_DROPDOWN_VISIBLE_ROWS ? count : SETTINGS_DROPDOWN_VISIBLE_ROWS;

		if (y >= overlay_y + 4 && y < overlay_y + 4 + visible * row_h && x >= L.x && x < L.x + L.field_w) {
			int row = (y - (overlay_y + 4)) / row_h;
			int opt_index = settings_dropdown_scroll[idx] + row;
			if (opt_index >= 0 && opt_index < count) {
				if (settings_open[idx] == SETTINGS_DROPDOWN_TIMEZONE) {
					settings_selected[idx] = opt_index;
					const struct timezone_option *opt = timezone_option_get(opt_index);
					if (opt) { rtc_set_timezone_offset_minutes(opt->offset_minutes); settings_applied[idx] = true; }
				} else {
					settings_scale_selected[idx] = opt_index;
					settings_apply_scale(idx);
				}
			}
			settings_open[idx] = SETTINGS_DROPDOWN_NONE;
			return;
		}
		/* clicked outside the open overlay: close it without changing
		 * the selection, same as clicking away from search/network */
		settings_open[idx] = SETTINGS_DROPDOWN_NONE;
		return;
	}

	if (x >= L.x && x < L.x + L.field_w) {
		if (y >= L.tz_field_y && y < L.tz_field_y + L.field_h) {
			settings_open[idx] = SETTINGS_DROPDOWN_TIMEZONE;
			settings_dropdown_scroll[idx] = settings_selected[idx];
			settings_applied[idx] = false;
		} else if (y >= L.scale_field_y && y < L.scale_field_y + L.field_h) {
			settings_open[idx] = SETTINGS_DROPDOWN_SCALE;
			settings_dropdown_scroll[idx] = settings_scale_selected[idx];
			settings_applied[idx] = false;
		}
	}
}

/* Settings' own natural size: wide enough for its longest fixed
 * description line or timezone label (whichever is longer), tall
 * enough for both dropdown sections plus the footer hint - all
 * measured from the actual strings via gfx_string_width() rather than
 * a guessed pixel constant, the same approach as About/Uptime/
 * Palette above. */
static void settings_natural_size(int *out_w, int *out_h) {
	int lh = gfx_char_height() + 4;
	int field_h = gfx_char_height() + 14;

	int max_w = gfx_string_width("network/GPS) - it assumes the clock is");
	int w2 = gfx_string_width("There's no real video-mode switch here,");
	if (w2 > max_w) max_w = w2;
	for (int i = 0; i < timezone_option_count(); i++) {
		int lw = gfx_string_width(timezone_label_at(i)) + 20; /* +20 for the chevron */
		if (lw > max_w) max_w = lw;
	}
	*out_w = max_w + 2 * PADDING;

	int h = lh + 6 + lh * 3 + 10 + field_h + 20 /* Timezone section */
	       + lh + 6 + lh * 2 + 10 + field_h     /* UI Scale section */
	       + 24 + PADDING;                       /* footer + trailing margin */
	*out_h = h;
}

/* --- Text Editor: a real window, like Palette, rather than a console
 * takeover. The actual editing logic (insert/delete/move/split) lives
 * in noteedit.c, shared with the full-screen console `nano` command -
 * this only draws it with gfx_* calls instead of console_* ones, and
 * adds the one thing a window needs that the console version didn't:
 * its own filename entry field, since there's no console to borrow
 * for that prompt anymore. Per-window state, indexed by window slot
 * (only one Text Editor window can exist at a time, same one-per-name
 * rule every app here follows). */
#define TEXTEDITOR_FILENAME_MAX 32
struct texteditor_state {
	bool picking_filename; /* true = showing the filename field; false = editing */
	char filename[TEXTEDITOR_FILENAME_MAX];
	int filename_len;
	struct note_buffer nb;
	bool loaded; /* true once note_load() has actually run for `filename` */
};
static struct texteditor_state texteditor[MAX_WINDOWS];

/* Text Editor's content (line numbers + arbitrary file text) is
 * inherently open-ended and already scrolls both directions, so
 * there's no single "natural size" that fits everything the way
 * About's fixed text does - this is a comfortable minimum in
 * character-cell units instead, wide/tall enough for real editing
 * without immediately needing to scroll, and scaling correctly with
 * the font the same way every other window here does. */
static void texteditor_natural_size(int *out_w, int *out_h) {
	int cw = gfx_char_width(), ch = gfx_char_height();
	*out_w = 65 * cw + 2 * PADDING;
	*out_h = 24 * (ch + 4) + 2 * PADDING;
}

static void paint_texteditor(struct window *w) {
	int idx = (int)(w - windows);
	struct texteditor_state *st = &texteditor[idx];
	int x = w->x + PADDING, y = w->y + TITLEBAR_H + PADDING;
	int lh = gfx_char_height() + 4;

	if (st->picking_filename) {
		gfx_draw_string(x, y, "Text Editor", w->accent); y += lh + 10;
		gfx_draw_string(x, y, "File to open (created if it doesn't", COL_TEXT_DIM); y += lh;
		gfx_draw_string(x, y, "exist), then press Enter:", COL_TEXT_DIM); y += lh + 10;

		int field_w = w->w - 2 * PADDING;
		int field_h = 32;
		gfx_fill_round_rect(x, y, field_w, field_h, 6, COL_WIN_BODY_ALT);
		gfx_draw_string(x + 10, y + (field_h - gfx_char_height()) / 2, st->filename, COL_TEXT);
		if (((timer_get_ticks() / 30) % 2) == 0) {
			int cursor_x = x + 10 + gfx_string_width(st->filename);
			gfx_fill_rect(cursor_x + 2, y + 6, 2, field_h - 12, w->accent);
		}
		return;
	}

	/* editing mode: header line, then a status line, then the
	 * line-numbered text - matching the console version's information
	 * but on two lines instead of one, so a long filename can never
	 * collide with the "Ln/Col" readout the way sharing one line risked. */
	char header[96];
	strcpy(header, st->filename);
	if (st->nb.dirty) strcat(header, " [modified]");
	gfx_draw_string(x, y, header, w->accent);
	y += lh + 2;

	char pos[48];
	pos[0] = '\0';
	{
		/* "Ln %d, Col %d" without kprintf (console-only) - build it by
		 * hand the same way wm.c's network panel formats numbers */
		char numbuf[12];
		int n, i;
		strcat(pos, "Ln ");
		n = st->nb.cur_line + 1; i = 11; numbuf[i] = '\0';
		if (n == 0) numbuf[--i] = '0';
		while (n > 0) { numbuf[--i] = (char)('0' + n % 10); n /= 10; }
		strcat(pos, &numbuf[i]);
		strcat(pos, ", Col ");
		n = st->nb.cur_col + 1; i = 11; numbuf[i] = '\0';
		if (n == 0) numbuf[--i] = '0';
		while (n > 0) { numbuf[--i] = (char)('0' + n % 10); n /= 10; }
		strcat(pos, &numbuf[i]);
	}
	gfx_draw_string(x, y, pos, COL_TEXT_DIM);
	y += lh + 8;
	int text_area_top = y; /* first text row's y - used below for the cursor's y math too, so it can't drift out of sync with the header layout above */

	int gutter_digits = note_line_number_digits(&st->nb);
	int char_w = gfx_char_width();
	int gutter_px = (gutter_digits + 3) * char_w;

	int row_h = lh;
	int max_visible = (w->h + TITLEBAR_H - (y - w->y) - PADDING) / row_h;
	if (max_visible < 1) max_visible = 1;

	int visible_start = 0;
	if (st->nb.cur_line >= visible_start + max_visible) visible_start = st->nb.cur_line - max_visible + 1;
	if (st->nb.cur_line < visible_start) visible_start = st->nb.cur_line;
	if (st->nb.line_count > max_visible && visible_start > st->nb.line_count - max_visible) {
		visible_start = st->nb.line_count - max_visible;
	}
	if (visible_start < 0) visible_start = 0;

	for (int i = visible_start; i < st->nb.line_count && i < visible_start + max_visible; i++) {
		char numbuf[16];
		int n = i + 1, p = 15;
		numbuf[p] = '\0';
		while (n > 0) { numbuf[--p] = (char)('0' + n % 10); n /= 10; }
		int num_w = gfx_string_width(&numbuf[p]);
		gfx_draw_string(x + gutter_px - char_w * 3 - num_w, y, &numbuf[p], COL_TEXT_DIM);
		gfx_draw_string(x + gutter_px - char_w * 2, y, "|", COL_TEXT_DIM);
		gfx_draw_string(x + gutter_px, y, st->nb.lines[i], COL_TEXT);
		y += row_h;
	}

	/* blinking text-entry cursor at the real (cur_line, cur_col)
	 * position, the windowed equivalent of console_set_cursor() */
	if (((timer_get_ticks() / 30) % 2) == 0) {
		int cursor_row = st->nb.cur_line - visible_start;
		if (cursor_row >= 0 && cursor_row < max_visible) {
			int cx = x + gutter_px + st->nb.cur_col * char_w;
			int cy = text_area_top + cursor_row * row_h;
			gfx_fill_rect(cx, cy, 2, gfx_char_height(), w->accent);
		}
	}
}

static void key_texteditor(struct window *w, char c) {
	int idx = (int)(w - windows);
	struct texteditor_state *st = &texteditor[idx];

	if (st->picking_filename) {
		if (c == '\n') {
			if (st->filename_len == 0) return;
			note_load(&st->nb, FAT16_ROOT_CLUSTER, st->filename);
			st->loaded = true;
			st->picking_filename = false;
		} else if (c == '\b') {
			if (st->filename_len > 0) st->filename[--st->filename_len] = '\0';
		} else if (c >= 32 && c < 127 && st->filename_len < TEXTEDITOR_FILENAME_MAX - 1) {
			st->filename[st->filename_len++] = c;
			st->filename[st->filename_len] = '\0';
		}
		return;
	}

	if (c == CTRL_KEY('s')) {
		note_save(&st->nb, FAT16_ROOT_CLUSTER, st->filename);
	} else {
		note_handle_key(&st->nb, c);
	}
}

/* --- Browser: a real (if very basic) web browser. Address bar ->
 * net_resolve_hostname() (DNS) -> net_http_get() or net_https_get()
 * (plain HTTP or TLS 1.2, by scheme) -> html_layout() (html.c) -> a
 * scrollable rendered page. See html.c's and tls.c's file comments
 * for the honest limitations (no CSS/images/tables/JS; no
 * certificate-chain validation). Per-window state, same one-slot-per-
 * open-window pattern as Text Editor. */
#define BROWSER_URL_MAX 192
#define BROWSER_HOST_MAX 128
#define BROWSER_PATH_MAX 192
#define BROWSER_BODY_MAX (64 * 1024)

struct browser_state {
	char url[BROWSER_URL_MAX];
	int url_len;
	bool editing_url; /* true while the address bar has focus (always, except while a page is displayed and the user is scrolling - Ctrl+L refocuses it) */

	bool has_page;
	struct html_page page;
	int scroll;

	bool loading;
	bool pending_load; /* true for exactly one frame: "Loading..." has been requested but the (blocking, possibly multi-second) fetch hasn't started yet - see wm_run()'s main loop, which checks this right after gfx_flip() so "Loading..." actually reaches the screen before the fetch blocks everything */
	bool has_error;
	char error[128];
};
static struct browser_state browser[MAX_WINDOWS];
static uint8_t browser_body[BROWSER_BODY_MAX]; /* one fetch at a time overall (this OS has no real concurrency) - shared, not per-window */

/* Splits "[http://|https://]host[:port][/path]" into its pieces.
 * Defaults: scheme http, port 80 (or 443 for https), path "/". No
 * userinfo, no query-string handling beyond passing it through as
 * part of the path, no IPv6 literals - a real but minimal URL parser. */
struct parsed_url {
	bool https;
	char host[BROWSER_HOST_MAX];
	uint16_t port;
	char path[BROWSER_PATH_MAX];
};

static bool browser_parse_url(const char *url, struct parsed_url *out) {
	out->https = false;
	out->port = 80;
	strcpy(out->path, "/");

	const char *p = url;
	if (strncmp(p, "https://", 8) == 0) { out->https = true; out->port = 443; p += 8; }
	else if (strncmp(p, "http://", 7) == 0) { p += 7; }

	if (*p == '\0') return false;

	uint32_t host_len = 0;
	while (*p && *p != '/' && *p != ':' && host_len < BROWSER_HOST_MAX - 1) {
		out->host[host_len++] = *p++;
	}
	out->host[host_len] = '\0';
	if (host_len == 0) return false;

	if (*p == ':') {
		p++;
		uint32_t port_val = 0;
		while (*p >= '0' && *p <= '9') { port_val = port_val * 10 + (uint32_t)(*p - '0'); p++; }
		if (port_val == 0 || port_val > 65535) return false;
		out->port = (uint16_t)port_val;
	}

	if (*p == '/') {
		uint32_t path_len = 0;
		while (*p && path_len < BROWSER_PATH_MAX - 1) out->path[path_len++] = *p++;
		out->path[path_len] = '\0';
	}

	return true;
}

static void browser_load(struct window *w) {
	int idx = (int)(w - windows);
	struct browser_state *st = &browser[idx];

	st->has_page = false;
	st->has_error = false;
	st->scroll = 0;

	struct parsed_url u;
	if (!browser_parse_url(st->url, &u)) {
		st->has_error = true;
		strcpy(st->error, "Invalid URL. Try: http://host/path or https://host/path");
		return;
	}

	uint32_t ip;
	char neterr[96];
	if (!net_resolve_hostname(u.host, &ip, neterr, sizeof(neterr))) {
		st->has_error = true;
		strcpy(st->error, neterr);
		return;
	}

	char ip_str[16];
	{
		const uint8_t *b = (const uint8_t *)&ip;
		int pos = 0;
		for (int i = 0; i < 4; i++) {
			uint8_t v = b[i];
			if (v >= 100) ip_str[pos++] = (char)('0' + v / 100);
			if (v >= 10) ip_str[pos++] = (char)('0' + (v / 10) % 10);
			ip_str[pos++] = (char)('0' + v % 10);
			if (i < 3) ip_str[pos++] = '.';
		}
		ip_str[pos] = '\0';
	}

	uint32_t body_len = 0;
	bool ok;
	if (u.https) {
		ok = net_https_get(ip, u.host, u.port, u.path, browser_body, sizeof(browser_body), &body_len, neterr, sizeof(neterr));
	} else {
		ok = net_http_get(ip_str, u.port, u.path, browser_body, sizeof(browser_body), &body_len, neterr, sizeof(neterr));
	}

	if (!ok) {
		st->has_error = true;
		strcpy(st->error, neterr);
		return;
	}

	int wrap_cols = (w->w - 2 * PADDING) / gfx_char_width();
	html_layout(browser_body, body_len, &st->page, wrap_cols);
	st->has_page = true;
}

/* Browser content is a rendered web page - inherently open-ended and
 * already scrollable, so like Text Editor this is a comfortable
 * minimum in character-cell units (wide enough that html.c's own
 * word-wrap produces reasonably-filled lines) rather than a measured
 * exact fit. */
static void browser_natural_size(int *out_w, int *out_h) {
	int cw = gfx_char_width(), ch = gfx_char_height();
	*out_w = 80 * cw + 2 * PADDING;
	*out_h = 26 * (ch + 4) + 2 * PADDING;
}

static void paint_browser(struct window *w) {
	int idx = (int)(w - windows);
	struct browser_state *st = &browser[idx];
	int x = w->x + PADDING, y = w->y + TITLEBAR_H + PADDING;
	int lh = gfx_char_height() + 4;

	/* address bar */
	int field_w = w->w - 2 * PADDING;
	int field_h = 30;
	gfx_fill_round_rect(x, y, field_w, field_h, 6, st->editing_url ? COL_WIN_BODY_ALT : COL_TASKBAR);
	const char *shown = st->url_len > 0 ? st->url : "Type a URL and press Enter (e.g. http://93.184.216.34/)";
	gfx_draw_string(x + 10, y + (field_h - gfx_char_height()) / 2, shown, st->url_len > 0 ? COL_TEXT : COL_TEXT_DIM);
	if (st->editing_url && ((timer_get_ticks() / 30) % 2) == 0) {
		int cursor_x = x + 10 + gfx_string_width(st->url);
		gfx_fill_rect(cursor_x + 2, y + 5, 2, field_h - 10, w->accent);
	}
	y += field_h + 10;

	if (st->loading) {
		gfx_draw_string(x, y, "Loading...", COL_TEXT_DIM);
		return;
	}

	if (st->has_error) {
		gfx_draw_string(x, y, "Error:", GFX_RGB(0xFF, 0x6B, 0x6B));
		y += lh + 4;
		gfx_draw_string(x, y, st->error, COL_TEXT_DIM);
		return;
	}

	if (!st->has_page) {
		gfx_draw_string(x, y, "Enter an address above, then press Enter.", COL_TEXT_DIM);
		return;
	}

	if (st->page.title[0]) {
		gfx_draw_string(x, y, st->page.title, w->accent);
		y += lh + 6;
	}

	int content_top = y;
	int max_visible = (w->h + TITLEBAR_H - (content_top - w->y) - PADDING) / lh;
	if (max_visible < 1) max_visible = 1;

	if (st->scroll > st->page.line_count - max_visible) st->scroll = st->page.line_count - max_visible;
	if (st->scroll < 0) st->scroll = 0;

	for (int i = st->scroll; i < st->page.line_count && i < st->scroll + max_visible; i++) {
		gfx_draw_string(x, y, st->page.lines[i], COL_TEXT);
		y += lh;
	}
}

static void key_browser(struct window *w, char c) {
	int idx = (int)(w - windows);
	struct browser_state *st = &browser[idx];

	if (c == CTRL_KEY('l')) {
		st->editing_url = true;
		return;
	}

	if (st->editing_url) {
		if (c == '\n') {
			if (st->url_len == 0) return;
			st->editing_url = false;
			st->loading = true;
			st->pending_load = true; /* actually fetched from wm_run(), once this "Loading..." state has had a chance to reach the screen */
		} else if (c == '\b') {
			if (st->url_len > 0) st->url[--st->url_len] = '\0';
		} else if (c >= 32 && c < 127 && st->url_len < BROWSER_URL_MAX - 1) {
			st->url[st->url_len++] = c;
			st->url[st->url_len] = '\0';
		}
		return;
	}

	if (c == KEY_ARROW_UP) { if (st->scroll > 0) st->scroll--; }
	else if (c == KEY_ARROW_DOWN) { st->scroll++; }
}

static int create_window(int x, int y, int w, int h, const char *title, window_paint_fn paint, window_key_fn key, window_click_fn click, gfx_color_t accent) {
	for (int i = 0; i < MAX_WINDOWS; i++) {
		if (!windows[i].used) {
			windows[i].used = true;
			windows[i].x = x;
			windows[i].y = y;
			windows[i].w = w;
			windows[i].h = h;
			windows[i].paint = paint;
			windows[i].key = key;
			windows[i].click = click;
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
	int x_offset, y_offset, w, h; /* w/h are a fallback minimum, used as-is only when natural_size is NULL */
	window_paint_fn paint;
	window_key_fn key; /* NULL for apps with no keyboard interaction (About, Uptime, Palette) */
	window_click_fn click; /* NULL for apps with no content-area click interaction */
	window_natural_size_fn natural_size; /* NULL to just use w/h as given */
	enum app_icon icon;
	gfx_color_t accent;
	/* Console apps (Terminal) aren't drawn as a window at all - they
	 * take over the whole screen via the software console the same way
	 * the shell does, so launching one suspends the WM loop entirely
	 * rather than creating a struct window. `paint`/`key`/geometry are
	 * unused (and left zeroed) for these. */
	bool is_console_app;
};

#define MAX_APPS 8
static struct app_entry apps[MAX_APPS];
static int app_count = 0;
static int base_cx, base_cy;

static void register_app(const char *name, int x_offset, int y_offset, int w, int h, window_paint_fn paint, window_natural_size_fn natural_size, enum app_icon icon, gfx_color_t accent) {
	if (app_count >= MAX_APPS) return;
	apps[app_count++] = (struct app_entry){
		.name = name, .x_offset = x_offset, .y_offset = y_offset, .w = w, .h = h,
		.paint = paint, .natural_size = natural_size, .icon = icon, .accent = accent,
	};
}

static void register_interactive_app(const char *name, int x_offset, int y_offset, int w, int h, window_paint_fn paint, window_key_fn key, window_click_fn click, window_natural_size_fn natural_size, enum app_icon icon, gfx_color_t accent) {
	if (app_count >= MAX_APPS) return;
	apps[app_count++] = (struct app_entry){
		.name = name, .x_offset = x_offset, .y_offset = y_offset, .w = w, .h = h,
		.paint = paint, .key = key, .click = click, .natural_size = natural_size, .icon = icon, .accent = accent,
	};
}

static void register_console_app(const char *name, enum app_icon icon) {
	if (app_count >= MAX_APPS) return;
	apps[app_count++] = (struct app_entry){ .name = name, .icon = icon, .is_console_app = true };
}

static int find_open_window_by_name(const char *name) {
	for (int i = 0; i < MAX_WINDOWS; i++) {
		if (windows[i].used && strcmp(windows[i].title, name) == 0) return i;
	}
	return -1;
}

/* Recomputes every currently-open window's size from its app's
 * natural_size function (see window_natural_size_fn's comment) and
 * resizes it in place, anchored at its current top-left corner -
 * called right after a UI-scale change (Settings' "UI Scale"
 * dropdown), since every app's fixed chrome text needs more or fewer
 * pixels at the new font scale to still fit without clipping.
 * Fullscreen windows are left as fullscreen (still covering the
 * screen); their saved restore_w/restore_h is updated instead, so
 * un-maximizing later restores to the newly correct size rather than
 * the old scale's. Windows are also nudged back on-screen if resizing
 * would otherwise push them past the screen edge. */
static void rescale_all_windows(void) {
	for (int i = 0; i < MAX_WINDOWS; i++) {
		struct window *w = &windows[i];
		if (!w->used) continue;

		window_natural_size_fn natural_size = NULL;
		for (int a = 0; a < app_count; a++) {
			if (strcmp(apps[a].name, w->title) == 0) { natural_size = apps[a].natural_size; break; }
		}
		if (!natural_size) continue;

		int new_w, new_h;
		natural_size(&new_w, &new_h);

		if (w->fullscreen) {
			w->restore_w = new_w;
			w->restore_h = new_h;
			continue;
		}

		w->w = new_w;
		w->h = new_h;
		if (w->x + new_w > screen_w) w->x = screen_w - new_w;
		if (w->x < 0) w->x = 0;
		if (w->y + new_h + TITLEBAR_H > screen_h - TASKBAR_H) w->y = screen_h - TASKBAR_H - new_h - TITLEBAR_H;
		if (w->y < 0) w->y = 0;
	}
}

static void run_console_app(void (*entry)(void)) {
	console_clear();
	gfx_set_font_scale(1);
	entry();
	gfx_set_font_scale(2);
	/* the console app owns the keyboard buffer while it runs; nothing
	 * queued during that time is meant for the desktop, so drop it
	 * rather than have leftover keystrokes fire WM shortcuts on return */
	while (keyboard_has_key()) keyboard_getchar_blocking();
}

/* Called exactly once, right after a fresh window is created, so apps
 * with real state (Settings' selected timezone, Text Editor's buffer)
 * start from a clean slate rather than whatever was left over in their
 * static per-slot arrays from a previous window that used the same
 * slot index. */
static void window_opened(int idx, const char *app_name) {
	if (strcmp(app_name, "Settings") == 0) {
		settings_selected[idx] = timezone_find_closest_option(rtc_get_timezone_offset_minutes());
		settings_scale_selected[idx] = ui_scale_find_option(gfx_font_scale());
		settings_applied[idx] = false;
		settings_open[idx] = SETTINGS_DROPDOWN_NONE;
		settings_dropdown_scroll[idx] = 0;
	} else if (strcmp(app_name, "Text Editor") == 0) {
		memset(&texteditor[idx], 0, sizeof(texteditor[idx]));
		texteditor[idx].picking_filename = true;
	} else if (strcmp(app_name, "Browser") == 0) {
		memset(&browser[idx], 0, sizeof(browser[idx]));
		browser[idx].editing_url = true;
	}
}

static void launch_or_focus_app(int app_index) {
	if (app_index < 0 || app_index >= app_count) return;
	struct app_entry *app = &apps[app_index];

	if (app->is_console_app) {
		if (strcmp(app->name, "Terminal") == 0) {
			run_console_app(terminal_app_run);
		}
		return;
	}

	int idx = find_open_window_by_name(app->name);
	if (idx < 0) {
		int w = app->w, h = app->h;
		if (app->natural_size) app->natural_size(&w, &h);
		idx = create_window(base_cx + app->x_offset, base_cy + app->y_offset, w, h, app->name, app->paint, app->key, app->click, app->accent);
		if (idx >= 0) window_opened(idx, app->name); /* let a freshly created window initialize its own per-window state */
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
	network_panel_open = false;
	netinfo_scan(); /* so the taskbar icon reflects real status immediately, not just after the first click */

	base_cx = screen_w / 2 - 340;
	base_cy = screen_h / 2 - 230;

	register_app("About auroraOS", 0, 0, 500, 320, paint_about, about_natural_size, ICON_INFO, COL_ACCENT);
	register_app("Uptime", 540, 0, 280, 190, paint_counter, counter_natural_size, ICON_CLOCK, GFX_RGB(0x28, 0xC8, 0x40));
	register_app("Palette", 100, 290, 320, 160, paint_palette, palette_natural_size, ICON_PALETTE, GFX_RGB(0xB1, 0x8C, 0xFF));
	register_interactive_app("Settings", 100, 40, 380, 330, paint_settings, key_settings, click_settings, settings_natural_size, ICON_GEAR, GFX_RGB(0x4D, 0xD0, 0xC7));
	register_interactive_app("Text Editor", 40, 20, 520, 400, paint_texteditor, key_texteditor, NULL, texteditor_natural_size, ICON_DOCUMENT, COL_ACCENT);
	register_interactive_app("Browser", 30, 10, 640, 460, paint_browser, key_browser, NULL, browser_natural_size, ICON_GLOBE, GFX_RGB(0xFF, 0x8A, 0x3D));
	register_console_app("Terminal", ICON_TERMINAL);

	/* Apps are registered so search/the taskbar can find them, but none
	 * are opened automatically - the desktop boots to an empty screen,
	 * same as a real OS, and the user opens what they want via search. */
}

static void draw_titlebar_button(int x, int y, gfx_color_t color) {
	gfx_fill_round_rect(x, y, 12, 12, 6, color);
}

/* Draws one app's icon glyph inside the (x, y, size, size) box, in
 * `color` (each call site picks the app's own accent, same as the old
 * flat-swatch behavior, just shaped now instead of a plain square).
 * Deliberately simple, chunky shapes built only from this file's
 * existing gfx_fill_rect/gfx_fill_round_rect/gfx_draw_line primitives
 * (a filled circle is just gfx_fill_round_rect with radius = size/2)
 * - legible at the small sizes these actually render at (14px in the
 * search list, similar in a future taskbar icon slot), not detailed
 * icon art. */
static void draw_app_icon(int x, int y, int size, enum app_icon icon, gfx_color_t color) {
	switch (icon) {
		case ICON_INFO: {
			int r = size / 2;
			gfx_fill_round_rect(x, y, size, size, r, color);
			int dot_size = size / 6 > 0 ? size / 6 : 1;
			gfx_fill_rect(x + r - dot_size / 2, y + size / 5, dot_size, dot_size, COL_WIN_BODY);
			gfx_fill_rect(x + r - dot_size / 2, y + size / 2 - dot_size / 2, dot_size, size / 3, COL_WIN_BODY);
			break;
		}
		case ICON_CLOCK: {
			int r = size / 2;
			gfx_fill_round_rect(x, y, size, size, r, color);
			int cx = x + r, cy = y + r;
			gfx_draw_line(cx, cy, cx, y + size / 5, COL_WIN_BODY);       /* minute hand, pointing up */
			gfx_draw_line(cx, cy, x + size * 2 / 3, cy, COL_WIN_BODY);   /* hour hand, pointing right */
			break;
		}
		case ICON_PALETTE: {
			int r = size / 2;
			gfx_fill_round_rect(x, y, size, size, r, color);
			int dot = size / 5 > 1 ? size / 5 : 2;
			gfx_fill_round_rect(x + size / 4 - dot / 2, y + size / 4 - dot / 2, dot, dot, dot / 2, GFX_RGB(0xFF, 0x5F, 0x57));
			gfx_fill_round_rect(x + size * 3 / 4 - dot / 2, y + size / 4 - dot / 2, dot, dot, dot / 2, GFX_RGB(0x28, 0xC8, 0x40));
			gfx_fill_round_rect(x + size / 2 - dot / 2, y + size * 3 / 4 - dot / 2, dot, dot, dot / 2, GFX_RGB(0x5B, 0x9C, 0xFF));
			break;
		}
		case ICON_GEAR: {
			int r = size / 2;
			int cx = x + r, cy = y + r;
			/* four teeth: small squares at N/S/E/W around a central ring */
			int tooth = size / 4 > 1 ? size / 4 : 1;
			gfx_fill_rect(cx - tooth / 2, y, tooth, tooth, color);
			gfx_fill_rect(cx - tooth / 2, y + size - tooth, tooth, tooth, color);
			gfx_fill_rect(x, cy - tooth / 2, tooth, tooth, color);
			gfx_fill_rect(x + size - tooth, cy - tooth / 2, tooth, tooth, color);
			gfx_fill_round_rect(x + size / 5, y + size / 5, size * 3 / 5, size * 3 / 5, size * 3 / 10, color);
			int hole = size / 4 > 1 ? size / 4 : 1;
			gfx_fill_round_rect(cx - hole / 2, cy - hole / 2, hole, hole, hole / 2, COL_WIN_BODY);
			break;
		}
		case ICON_DOCUMENT: {
			int fold = size / 3;
			gfx_fill_round_rect(x, y, size, size, 3, color);
			gfx_fill_rect(x + size - fold, y, fold, fold, COL_WIN_BODY); /* dog-eared corner cutout */
			int line_y = y + size / 2;
			for (int i = 0; i < 3; i++) {
				gfx_fill_rect(x + size / 5, line_y + i * (size / 6), size * 3 / 5, 1, COL_WIN_BODY);
			}
			break;
		}
		case ICON_GLOBE: {
			int r = size / 2;
			int cx = x + r, cy = y + r;
			gfx_fill_round_rect(x, y, size, size, r, color);
			gfx_fill_rect(x, cy, size, 1, COL_WIN_BODY); /* equator */
			gfx_fill_rect(cx, y, 1, size, COL_WIN_BODY); /* prime meridian */
			gfx_draw_line(x + size / 6, y + size / 4, x + size * 5 / 6, y + size / 4, COL_WIN_BODY);
			gfx_draw_line(x + size / 6, y + size * 3 / 4, x + size * 5 / 6, y + size * 3 / 4, COL_WIN_BODY);
			break;
		}
		case ICON_TERMINAL: {
			gfx_fill_round_rect(x, y, size, size, 3, color);
			int py = y + size / 3;
			gfx_draw_line(x + size / 5, py, x + size / 2, y + size / 2, COL_WIN_BODY);
			gfx_draw_line(x + size / 5, py + size / 3, x + size / 2, y + size / 2, COL_WIN_BODY);
			gfx_fill_rect(x + size / 2, y + size * 2 / 3, size / 3, 1, COL_WIN_BODY);
			break;
		}
		case ICON_NONE:
		default:
			gfx_fill_round_rect(x, y, size, size, 4, color);
			break;
	}
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

/* Small ascending-bars network icon (like any OS's connectivity
 * indicator), colored by real detected hardware status rather than a
 * fake connection state: dim gray with no hardware found, accent blue
 * if a network controller was actually enumerated over PCI. There's no
 * "connected" state to show at all - see netinfo.c's file comment on
 * why that would be dishonest to fake. */
static void draw_network_glyph(int cx, int cy) {
	struct netinfo_status status;
	netinfo_get(&status);
	gfx_color_t color = status.hardware_found ? COL_ACCENT : COL_TEXT_DIM;

	int base_y = cy + 6;
	int bar_w = 3, gap = 2;
	int heights[4] = { 3, 6, 9, 12 };
	int x = cx - 7;
	for (int i = 0; i < 4; i++) {
		gfx_fill_rect(x, base_y - heights[i], bar_w, heights[i], color);
		x += bar_w + gap;
	}
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

	/* network status button - real PCI-detected hardware, honestly no
	 * connectivity behind it (see netinfo.c) */
	network_btn_x = search_btn_x + search_btn_w + 12;
	network_btn_y = y + 8;
	network_btn_w = TASKBAR_H - 16;
	network_btn_h = TASKBAR_H - 16;
	gfx_fill_round_rect(network_btn_x, network_btn_y, network_btn_w, network_btn_h, 8,
		network_panel_open ? COL_TASKBAR_ACTIVE : GFX_RGB(0x1C, 0x1F, 0x2C));
	draw_network_glyph(network_btn_x + network_btn_w / 2, network_btn_y + network_btn_h / 2);

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

	int bx = network_btn_x + network_btn_w + 12;
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

		enum app_icon icon = ICON_NONE;
		for (int a = 0; a < app_count; a++) {
			if (strcmp(apps[a].name, w->title) == 0) { icon = apps[a].icon; break; }
		}

		int icon_size = TASKBAR_H - 26;
		int tw = gfx_string_width(w->title) + 32 + icon_size + 6;
		if (bx + tw > bx_max) break; /* out of room; skip remaining buttons */
		bool active = (idx == topmost);
		gfx_color_t text_color = w->minimized ? COL_TEXT_DIM : (active ? COL_TEXT : COL_TEXT_DIM);
		gfx_fill_round_rect(bx, y + 8, tw, TASKBAR_H - 16, 8,
			active ? COL_TASKBAR_ACTIVE : (w->minimized ? GFX_RGB(0x15, 0x17, 0x22) : GFX_RGB(0x1C, 0x1F, 0x2C)));
		if (active) gfx_fill_rect(bx + 8, y + TASKBAR_H - 6, tw - 16, 2, w->accent);
		draw_app_icon(bx + 12, y + (TASKBAR_H - icon_size) / 2, icon_size, icon, w->accent);
		int text_x = bx + 12 + icon_size + 6;
		if (w->minimized) {
			/* small dash icon hints "this is minimized, click to restore" */
			gfx_fill_rect(text_x, y + TASKBAR_H / 2 - 1, 8, 2, COL_TEXT_DIM);
			gfx_draw_string(text_x + 14, y + (TASKBAR_H - gfx_char_height()) / 2, w->title, text_color);
		} else {
			gfx_draw_string(text_x, y + (TASKBAR_H - gfx_char_height()) / 2, w->title, text_color);
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
		draw_app_icon(field_x + 8, row_y + (SEARCH_ROW_H - 4 - 14) / 2, 14, app->icon, app->accent);
		gfx_draw_string(field_x + 32, row_y + (SEARCH_ROW_H - 4 - gfx_char_height()) / 2, app->name, selected ? COL_TEXT : COL_TEXT_DIM);
		row_y += SEARCH_ROW_H;
	}
}

#define NETWORK_PANEL_W 300

static const char *format_mac(const uint8_t mac[6]) {
	static char buf[18]; /* "xx:xx:xx:xx:xx:xx\0" */
	const char *hex = "0123456789abcdef";
	int pos = 0;
	for (int i = 0; i < 6; i++) {
		buf[pos++] = hex[mac[i] >> 4];
		buf[pos++] = hex[mac[i] & 0xF];
		if (i < 5) buf[pos++] = ':';
	}
	buf[pos] = '\0';
	return buf;
}

/* Panel has three real states, not a fake "Connected" toggle:
 *   1. no network controller detected at all
 *   2. one detected, but it's a chipset auroraOS has no driver for
 *      (anything other than the e1000 - see e1000.c's file comment on
 *      why only that one chipset is supported)
 *   3. an e1000 - shows a real "Connect" button that runs an actual
 *      DHCP exchange (see net.c) and displays the real lease it gets
 *      back, or the real reason it failed */
static void draw_network_panel(void) {
	if (!network_panel_open) return;

	struct netinfo_status status;
	netinfo_get(&status);

	struct net_status net;
	bool have_lease = net_get_status(&net);

	int panel_h;
	if (!status.hardware_found) panel_h = 128;
	else if (!status.driver_supported) panel_h = 154;
	else if (have_lease) panel_h = 222;
	else if (connect_attempted) panel_h = 188;
	else panel_h = 158;

	int panel_x = network_btn_x;
	int panel_y = screen_h - TASKBAR_H - 12 - panel_h;

	gfx_draw_soft_shadow(panel_x, panel_y, NETWORK_PANEL_W, panel_h, CORNER_RADIUS, 12);
	gfx_fill_round_rect(panel_x, panel_y, NETWORK_PANEL_W, panel_h, CORNER_RADIUS, COL_WIN_BODY);
	gfx_draw_rect(panel_x, panel_y, NETWORK_PANEL_W, panel_h, GFX_RGB(0x00, 0x00, 0x00));

	int x = panel_x + 16, y = panel_y + 16;
	int lh = gfx_char_height() + 6;

	gfx_draw_string(x, y, "Network", COL_ACCENT); y += lh + 6;

	if (!status.hardware_found) {
		gfx_draw_string(x, y, "No network controller found.", COL_TEXT); y += lh + 10;
		gfx_draw_string(x, y, "auroraOS checks real PCI hardware -", COL_TEXT_DIM); y += lh;
		gfx_draw_string(x, y, "this machine/VM has none attached,", COL_TEXT_DIM); y += lh;
		gfx_draw_string(x, y, "or it's a kind not yet recognized.", COL_TEXT_DIM);
		connect_btn_w = 0; /* no button in this state */
		return;
	}

	gfx_draw_string(x, y, status.is_wireless ? "Wireless adapter detected:" : "Wired adapter detected:", COL_TEXT_DIM); y += lh;
	gfx_draw_string(x, y, status.name, COL_TEXT); y += lh + 10;

	if (!status.driver_supported) {
		gfx_draw_string(x, y, "No driver for this chipset - only the", COL_TEXT_DIM); y += lh;
		gfx_draw_string(x, y, "Intel e1000 is supported right now.", COL_TEXT_DIM);
		connect_btn_w = 0;
		return;
	}

	if (have_lease) {
		gfx_draw_string(x, y, "Connected (DHCP lease obtained)", GFX_RGB(0x28, 0xC8, 0x40)); y += lh + 8;
		char line[64];

		strcpy(line, "IP:      "); strcat(line, net.ip_str);
		gfx_draw_string(x, y, line, COL_TEXT); y += lh;

		strcpy(line, "Subnet:  "); strcat(line, net.subnet_str);
		gfx_draw_string(x, y, line, COL_TEXT); y += lh;

		strcpy(line, "Gateway: "); strcat(line, net.gateway_str);
		gfx_draw_string(x, y, line, COL_TEXT); y += lh;

		strcpy(line, "MAC:     "); strcat(line, format_mac(net.mac));
		gfx_draw_string(x, y, line, COL_TEXT_DIM);

		connect_btn_w = 0; /* already connected - no button needed */
		return;
	}

	if (connect_attempted) {
		gfx_draw_string(x, y, "Connection failed:", GFX_RGB(0xFF, 0x6B, 0x6B)); y += lh;
		gfx_draw_string(x, y, net.message, COL_TEXT_DIM); y += lh + 6;
	}

	connect_btn_x = x;
	connect_btn_y = y;
	connect_btn_w = NETWORK_PANEL_W - 32;
	connect_btn_h = 32;
	gfx_fill_round_rect(connect_btn_x, connect_btn_y, connect_btn_w, connect_btn_h, 6,
		connecting_in_progress ? COL_TASKBAR_ACTIVE : COL_ACCENT);
	const char *btn_label = connecting_in_progress ? "Connecting..." : "Connect (DHCP)";
	int label_w = gfx_string_width(btn_label);
	gfx_draw_string(connect_btn_x + (connect_btn_w - label_w) / 2,
		connect_btn_y + (connect_btn_h - gfx_char_height()) / 2, btn_label,
		connecting_in_progress ? COL_TEXT_DIM : COL_BLACK);
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

	if (point_in(x, y, network_btn_x, network_btn_y, network_btn_w, network_btn_h)) {
		network_panel_open = !network_panel_open;
		if (network_panel_open) netinfo_scan(); /* refresh on open - cheap, and hardware could differ each boot */
		return;
	}
	if (network_panel_open) {
		if (connect_btn_w > 0 && !connecting_in_progress &&
		    point_in(x, y, connect_btn_x, connect_btn_y, connect_btn_w, connect_btn_h)) {
			/* Redraw once with the "Connecting..." state showing before
			 * the blocking DHCP exchange runs (net_init_and_request_lease
			 * can take up to ~6 seconds - see net.c), so the button
			 * doesn't just silently freeze with no feedback. */
			connecting_in_progress = true;
			draw_taskbar();
			draw_network_panel();
			gfx_flip();

			net_init_and_request_lease();
			connect_attempted = true;
			connecting_in_progress = false;
			return;
		}
		network_panel_open = false; /* clicked elsewhere: close it, same as search */
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
		return;
	}

	if (w->click) w->click(w, x - w->x, y - w->y);
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
		draw_network_panel();
		draw_cursor(mx, my);
		gfx_flip();

		/* Browser fetches are blocking (DNS + TCP/TLS handshake + HTTP,
		 * genuinely multi-second over a slow/real connection) and this
		 * whole WM loop is single-threaded/synchronous, so triggering one
		 * straight from key_browser() would freeze the UI on the exact
		 * same frame that was supposed to show "Loading..." - it'd never
		 * actually reach the screen. Doing it here instead, right after
		 * the frame with that text has already been flipped, means the
		 * user really does see "Loading..." for at least one frame before
		 * the freeze. */
		for (int i = 0; i < MAX_WINDOWS; i++) {
			if (windows[i].used && browser[i].pending_load) {
				browser[i].pending_load = false;
				browser_load(&windows[i]);
				browser[i].loading = false;
			}
		}

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
				} else if (c == KEY_ARROW_DOWN) {
					int matches[MAX_APPS];
					int match_count = search_matches(matches);
					if (search_selected < match_count - 1) search_selected++;
				} else if (c == KEY_ARROW_UP) {
					if (search_selected > 0) search_selected--;
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
			} else if (c == KEY_SUPER) {
				open_search();
			} else if (topmost_visible >= 0 && windows[topmost_visible].key) {
				/* Global shortcuts above always win even while a window
				 * has focus (Ctrl+Q must never feel "stuck" behind
				 * whatever's being edited) - only once none of them
				 * match does the topmost window get a chance to handle
				 * the key itself (Settings' arrows, Text Editor's
				 * typing). Only apps that registered a key handler via
				 * register_interactive_app() ever receive one. */
				windows[topmost_visible].key(&windows[topmost_visible], c);
			}
		}
	}
}
