/* settings.c - a small full-screen Settings app (see wm.c's "Settings"
 * console app entry), the same shape as the Text Editor/Terminal apps:
 * it takes over the console, does its thing, and hands control back to
 * the desktop on exit.
 *
 * First (and so far only) setting: timezone. There is no way for a
 * freestanding kernel on real x86 hardware to actually detect the
 * user's geographic region - no network stack, no GPS, and the CMOS
 * RTC itself has no timezone field, just a wall-clock (see rtc.c's file
 * comment). So "auto-detect" here is deliberately honest about what's
 * actually possible: the RTC is assumed to already show local time,
 * which is the default (UTC+0:00 offset applied, i.e. trust the RTC
 * as-is) until the user picks their real UTC offset from this list. */
#include "kernel.h"

struct timezone_option {
	const char *label;
	int offset_minutes;
};

/* A representative set of named UTC offsets - not an exhaustive IANA
 * zone database (there's no room or need for one here), just enough
 * for a real person to find their own offset by name at a glance. */
static const struct timezone_option timezone_options[] = {
	{ "UTC-12:00 (Baker Island)",       -12 * 60 },
	{ "UTC-11:00 (Samoa)",              -11 * 60 },
	{ "UTC-10:00 (Hawaii)",             -10 * 60 },
	{ "UTC-09:00 (Alaska)",              -9 * 60 },
	{ "UTC-08:00 (Pacific)",             -8 * 60 },
	{ "UTC-07:00 (Mountain)",            -7 * 60 },
	{ "UTC-06:00 (Central)",             -6 * 60 },
	{ "UTC-05:00 (Eastern)",             -5 * 60 },
	{ "UTC-04:00 (Atlantic)",            -4 * 60 },
	{ "UTC-03:00 (Buenos Aires)",        -3 * 60 },
	{ "UTC-02:00 (Mid-Atlantic)",        -2 * 60 },
	{ "UTC-01:00 (Azores)",              -1 * 60 },
	{ "UTC+00:00 (UTC / London)",             0 },
	{ "UTC+01:00 (Berlin / Paris)",       1 * 60 },
	{ "UTC+02:00 (Cairo / Athens)",       2 * 60 },
	{ "UTC+03:00 (Moscow / Nairobi)",     3 * 60 },
	{ "UTC+03:30 (Tehran)",          3 * 60 + 30 },
	{ "UTC+04:00 (Dubai)",                4 * 60 },
	{ "UTC+05:00 (Karachi)",              5 * 60 },
	{ "UTC+05:30 (India)",           5 * 60 + 30 },
	{ "UTC+06:00 (Dhaka)",                6 * 60 },
	{ "UTC+07:00 (Bangkok)",              7 * 60 },
	{ "UTC+08:00 (Beijing / Singapore)",  8 * 60 },
	{ "UTC+09:00 (Tokyo)",                9 * 60 },
	{ "UTC+09:30 (Adelaide)",        9 * 60 + 30 },
	{ "UTC+10:00 (Sydney)",              10 * 60 },
	{ "UTC+12:00 (Auckland)",            12 * 60 },
	{ "UTC+14:00 (Kiribati)",            14 * 60 },
};
#define TIMEZONE_OPTION_COUNT (int)(sizeof(timezone_options) / sizeof(timezone_options[0]))

static int find_closest_option(int offset_minutes) {
	int best = 0;
	int best_diff = timezone_options[0].offset_minutes - offset_minutes;
	if (best_diff < 0) best_diff = -best_diff;
	for (int i = 1; i < TIMEZONE_OPTION_COUNT; i++) {
		int diff = timezone_options[i].offset_minutes - offset_minutes;
		if (diff < 0) diff = -diff;
		if (diff < best_diff) { best = i; best_diff = diff; }
	}
	return best;
}

static void draw_timezone_picker(int selected) {
	console_clear();
	console_set_color(console_color_accent());
	console_writestring("-- Settings: Timezone --\n\n");
	console_set_color(console_color_default());
	console_writestring("auroraOS can't detect your region automatically (no network,\n");
	console_writestring("no GPS) - it assumes the system clock is already local time.\n");
	console_writestring("Pick your UTC offset below if the clock looks wrong.\n\n");
	console_set_color(console_color_dim());
	console_writestring("(arrows move, Enter selects, Esc cancels)\n\n");

	/* keep the visible window centered on the current selection, the
	 * same scrolling idea nano's gutter view uses */
	int max_visible = 20;
	int visible_start = 0;
	if (TIMEZONE_OPTION_COUNT > max_visible) {
		visible_start = selected - max_visible / 2;
		if (visible_start < 0) visible_start = 0;
		if (visible_start > TIMEZONE_OPTION_COUNT - max_visible) {
			visible_start = TIMEZONE_OPTION_COUNT - max_visible;
		}
	}

	int visible_end = visible_start + max_visible;
	if (visible_end > TIMEZONE_OPTION_COUNT) visible_end = TIMEZONE_OPTION_COUNT;

	for (int i = visible_start; i < visible_end; i++) {
		if (i == selected) {
			console_set_color(console_color_accent());
			console_writestring("> ");
		} else {
			console_set_color(console_color_default());
			console_writestring("  ");
		}
		console_writestring(timezone_options[i].label);
		console_putchar('\n');
	}
	console_set_color(console_color_default());
	console_present();
}

/* Entry point for launching Settings as a GUI app (see wm.c's
 * "Settings" console app entry). wm.c is responsible for switching
 * graphics modes before/after calling this, the same way it already
 * does for Text Editor and Terminal. */
void settings_app_run(void) {
	int selected = find_closest_option(rtc_get_timezone_offset_minutes());
	draw_timezone_picker(selected);

	for (;;) {
		char c = keyboard_getchar_blocking();

		if (c == KEY_ARROW_UP) {
			if (selected > 0) selected--;
			draw_timezone_picker(selected);
		} else if (c == KEY_ARROW_DOWN) {
			if (selected < TIMEZONE_OPTION_COUNT - 1) selected++;
			draw_timezone_picker(selected);
		} else if (c == '\n') {
			rtc_set_timezone_offset_minutes(timezone_options[selected].offset_minutes);
			console_clear();
			console_set_color(console_color_accent());
			console_writestring("Timezone set to ");
			console_writestring(timezone_options[selected].label);
			console_writestring(".\n");
			console_set_color(console_color_default());
			console_present();
			return;
		} else if (c == 27 || c == CTRL_KEY('x')) { /* Escape or Ctrl+X */
			console_clear();
			console_writestring("Cancelled - timezone unchanged.\n");
			console_present();
			return;
		}
	}
}
