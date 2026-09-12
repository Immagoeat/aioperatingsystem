/* wallpaper.c - registry of baked-in wallpaper images (raw RGB byte
 * arrays generated at build time by tools/img_to_c.py from
 * assets/wallpapers/, see kernel/generated/). Draws whichever one is
 * selected, scaled to the actual runtime screen resolution. */
#include "kernel.h"

extern const unsigned char wallpaper_day_rgb[];
extern const int wallpaper_day_width;
extern const int wallpaper_day_height;

extern const unsigned char wallpaper_night_rgb[];
extern const int wallpaper_night_width;
extern const int wallpaper_night_height;

static struct wallpaper wallpapers[2];
static bool initialized = false;

static void ensure_init(void) {
	if (initialized) return;
	wallpapers[0] = (struct wallpaper){ "Aurora Day", wallpaper_day_rgb, wallpaper_day_width, wallpaper_day_height };
	wallpapers[1] = (struct wallpaper){ "Aurora Night", wallpaper_night_rgb, wallpaper_night_width, wallpaper_night_height };
	initialized = true;
}

#define WALLPAPER_COUNT 2

int wallpaper_count(void) {
	return WALLPAPER_COUNT;
}

const struct wallpaper *wallpaper_get(int index) {
	ensure_init();
	if (index < 0 || index >= WALLPAPER_COUNT) return 0;
	return &wallpapers[index];
}

void wallpaper_draw(int index, int dst_w, int dst_h) {
	const struct wallpaper *wp = wallpaper_get(index);
	if (!wp) return;
	gfx_blit_rgb_scaled(wp->rgb, wp->width, wp->height, 0, 0, dst_w, dst_h);
}
