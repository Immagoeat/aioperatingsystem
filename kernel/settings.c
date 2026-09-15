/* settings.c - timezone data + logic for the windowed "Settings" app
 * (see wm.c's paint_settings()/key_settings(), a real window like
 * Palette rather than a console takeover).
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
#define TIMEZONE_OPTION_COUNT_VALUE (int)(sizeof(timezone_options) / sizeof(timezone_options[0]))

int timezone_option_count(void) {
	return TIMEZONE_OPTION_COUNT_VALUE;
}

const struct timezone_option *timezone_option_get(int index) {
	if (index < 0 || index >= TIMEZONE_OPTION_COUNT_VALUE) return NULL;
	return &timezone_options[index];
}

int timezone_find_closest_option(int offset_minutes) {
	int best = 0;
	int best_diff = timezone_options[0].offset_minutes - offset_minutes;
	if (best_diff < 0) best_diff = -best_diff;
	for (int i = 1; i < TIMEZONE_OPTION_COUNT_VALUE; i++) {
		int diff = timezone_options[i].offset_minutes - offset_minutes;
		if (diff < 0) diff = -diff;
		if (diff < best_diff) { best = i; best_diff = diff; }
	}
	return best;
}

/* Second (and so far last) setting: UI scale. There's no real video-
 * mode-switching driver here (the framebuffer resolution is fixed at
 * boot by whatever VBE mode GRUB requested - see gfx.c's file
 * comment), so a genuine "resolution" setting that changes actual
 * hardware pixel dimensions isn't feasible without a lot more work (a
 * real-mode VESA BIOS call or GOP runtime switch). This is the honest
 * stand-in: it scales the whole desktop's font/UI metrics (via
 * gfx_set_font_scale(), which every text and window-sizing
 * measurement in wm.c already derives from), which is a real and
 * useful "make everything bigger/smaller" control even though it
 * isn't literally switching video modes. Same session-only, reset-at-
 * every-boot scoping as the timezone setting above (see rtc.c's
 * timezone_offset_minutes) - no persistent settings storage exists in
 * this kernel yet. */
static const struct ui_scale_option ui_scale_options[] = {
	{ "Small", 1 },
	{ "Medium (default)", 2 },
	{ "Large", 3 },
};
#define UI_SCALE_OPTION_COUNT_VALUE (int)(sizeof(ui_scale_options) / sizeof(ui_scale_options[0]))

int ui_scale_option_count(void) {
	return UI_SCALE_OPTION_COUNT_VALUE;
}

const struct ui_scale_option *ui_scale_option_get(int index) {
	if (index < 0 || index >= UI_SCALE_OPTION_COUNT_VALUE) return NULL;
	return &ui_scale_options[index];
}

int ui_scale_find_option(int scale) {
	for (int i = 0; i < UI_SCALE_OPTION_COUNT_VALUE; i++) {
		if (ui_scale_options[i].scale == scale) return i;
	}
	return 1; /* "Medium (default)" */
}
