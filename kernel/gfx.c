/* gfx.c - true-color linear-framebuffer graphics library.
 *
 * Targets whatever VBE mode GRUB set up for us via the multiboot video
 * mode request in boot.s (see memory_get_framebuffer()). Colors are plain
 * 0xRRGGBB values; gfx_flip() packs them to the framebuffer's actual pixel
 * format (typically 32bpp XRGB, but this handles 24bpp too) each frame. */
#include "kernel.h"

#define MAX_FB_W 1920
#define MAX_FB_H 1080

static uint8_t *fb_addr;
static uint32_t fb_pitch;
static uint32_t fb_bpp;
static uint8_t fb_red_pos, fb_green_pos, fb_blue_pos;
/* True when the hardware's 32bpp pixel layout already matches
 * gfx_color_t's own in-memory format (0x00RRGGBB, red in bits 16-23,
 * green in 8-15, blue in 0-7) - the near-universal case for 32bpp VBE
 * modes (see memory.c's fallback comment). When it holds, gfx_flip()
 * can copy each row byte-for-byte instead of unpacking and repacking
 * every pixel, which is what it otherwise has to do on every single
 * frame regardless of whether anything on screen changed. */
static bool fb_is_native_rgb888;

static int screen_w = 1024;
static int screen_h = 768;

/* Back buffer: one 32-bit color per pixel, blitted to the real framebuffer
 * (in its native format) by gfx_flip(). Static allocation sized for the
 * largest resolution we'll realistically be handed. */
static gfx_color_t back_buffer[MAX_FB_W * MAX_FB_H];

bool gfx_init(void) {
	struct fb_info fb;
	if (!memory_get_framebuffer(&fb)) return false;
	if (fb.bpp != 32 && fb.bpp != 24) return false;
	if (fb.width > MAX_FB_W || fb.height > MAX_FB_H) return false;

	fb_addr = (uint8_t *)(uintptr_t)fb.addr;
	fb_pitch = fb.pitch;
	fb_bpp = fb.bpp;
	fb_red_pos = fb.red_pos;
	fb_green_pos = fb.green_pos;
	fb_blue_pos = fb.blue_pos;
	fb_is_native_rgb888 = (fb_bpp == 32 && fb_red_pos == 16 && fb_green_pos == 8 && fb_blue_pos == 0);

	screen_w = (int)fb.width;
	screen_h = (int)fb.height;

	memset(back_buffer, 0, sizeof(gfx_color_t) * (size_t)screen_w * (size_t)screen_h);
	return true;
}

int gfx_width(void) { return screen_w; }
int gfx_height(void) { return screen_h; }

static inline gfx_color_t blend(gfx_color_t bg, gfx_color_t fg, uint8_t alpha) {
	if (alpha == 255) return fg;
	if (alpha == 0) return bg;
	uint32_t br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
	uint32_t fr = (fg >> 16) & 0xFF, fgc = (fg >> 8) & 0xFF, fb_ = fg & 0xFF;
	uint32_t r = (fr * alpha + br * (255 - alpha)) / 255;
	uint32_t g = (fgc * alpha + bgc * (255 - alpha)) / 255;
	uint32_t b = (fb_ * alpha + bb * (255 - alpha)) / 255;
	return (r << 16) | (g << 8) | b;
}

void gfx_putpixel(int x, int y, gfx_color_t color) {
	if (x < 0 || y < 0 || x >= screen_w || y >= screen_h) return;
	back_buffer[y * screen_w + x] = color;
}

gfx_color_t gfx_getpixel(int x, int y) {
	if (x < 0 || y < 0 || x >= screen_w || y >= screen_h) return 0;
	return back_buffer[y * screen_w + x];
}

void gfx_blend_pixel(int x, int y, gfx_color_t color, uint8_t alpha) {
	if (x < 0 || y < 0 || x >= screen_w || y >= screen_h) return;
	gfx_color_t *p = &back_buffer[y * screen_w + x];
	*p = blend(*p, color, alpha);
}

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color) {
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + w; if (x1 > screen_w) x1 = screen_w;
	int y1 = y + h; if (y1 > screen_h) y1 = screen_h;
	for (int yy = y0; yy < y1; yy++) {
		gfx_color_t *row = &back_buffer[yy * screen_w + x0];
		for (int xx = 0; xx < x1 - x0; xx++) row[xx] = color;
	}
}

void gfx_blend_rect(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha) {
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + w; if (x1 > screen_w) x1 = screen_w;
	int y1 = y + h; if (y1 > screen_h) y1 = screen_h;
	for (int yy = y0; yy < y1; yy++) {
		for (int xx = x0; xx < x1; xx++) {
			gfx_color_t *p = &back_buffer[yy * screen_w + xx];
			*p = blend(*p, color, alpha);
		}
	}
}

/* Rounded rectangle via a per-corner radius test; cheap enough at these
 * resolutions and gives windows/buttons a modern, non-blocky silhouette. */
void gfx_fill_round_rect(int x, int y, int w, int h, int radius, gfx_color_t color) {
	if (radius <= 0) { gfx_fill_rect(x, y, w, h, color); return; }
	if (radius * 2 > w) radius = w / 2;
	if (radius * 2 > h) radius = h / 2;

	for (int yy = 0; yy < h; yy++) {
		int inset = 0;
		if (yy < radius) {
			int dy = radius - yy;
			/* integer sqrt via simple search is fine at this scale */
			int dx = 0;
			while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius) dx++;
			inset = radius - dx;
		} else if (yy >= h - radius) {
			int dy = yy - (h - radius) + 1;
			int dx = 0;
			while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius) dx++;
			inset = radius - dx;
		}
		gfx_fill_rect(x + inset, y + yy, w - 2 * inset, 1, color);
	}
}

void gfx_blend_round_rect(int x, int y, int w, int h, int radius, gfx_color_t color, uint8_t alpha) {
	if (radius <= 0) { gfx_blend_rect(x, y, w, h, color, alpha); return; }
	if (radius * 2 > w) radius = w / 2;
	if (radius * 2 > h) radius = h / 2;

	for (int yy = 0; yy < h; yy++) {
		int inset = 0;
		if (yy < radius) {
			int dy = radius - yy;
			int dx = 0;
			while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius) dx++;
			inset = radius - dx;
		} else if (yy >= h - radius) {
			int dy = yy - (h - radius) + 1;
			int dx = 0;
			while ((dx + 1) * (dx + 1) + dy * dy <= radius * radius) dx++;
			inset = radius - dx;
		}
		gfx_blend_rect(x + inset, y + yy, w - 2 * inset, 1, color, alpha);
	}
}

/* Soft drop shadow: several nested translucent rounded rects, blurring
 * outward. Cheap "fake gaussian" that reads fine at desktop scale. */
void gfx_draw_soft_shadow(int x, int y, int w, int h, int radius, int spread) {
	for (int i = spread; i > 0; i--) {
		uint8_t alpha = (uint8_t)(18 - (14 * i) / spread);
		if (alpha < 1) alpha = 1;
		gfx_blend_round_rect(x - i, y - i, w + i * 2, h + i * 2, radius + i, 0x000000, alpha);
	}
}

void gfx_draw_rect(int x, int y, int w, int h, gfx_color_t color) {
	gfx_fill_rect(x, y, w, 1, color);
	gfx_fill_rect(x, y + h - 1, w, 1, color);
	gfx_fill_rect(x, y, 1, h, color);
	gfx_fill_rect(x + w - 1, y, 1, h, color);
}

void gfx_draw_hline(int x, int y, int w, gfx_color_t color) {
	gfx_fill_rect(x, y, w, 1, color);
}

void gfx_draw_vline(int x, int y, int h, gfx_color_t color) {
	gfx_fill_rect(x, y, 1, h, color);
}

void gfx_draw_line(int x0, int y0, int x1, int y1, gfx_color_t color) {
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

static int glyph_scale = 2;

void gfx_set_font_scale(int scale) {
	glyph_scale = scale < 1 ? 1 : scale;
}

void gfx_draw_char(int x, int y, char c, gfx_color_t fg) {
	const uint8_t *glyph = font8x8_get_glyph(c);
	int size = 8 * glyph_scale;

	/* Fast path: the glyph's whole bounding box is on-screen (true for
	 * the overwhelming majority of characters drawn - console text and
	 * every window's labels), so each set pixel/block can be written
	 * straight into the back buffer instead of going through
	 * gfx_fill_rect()'s clamp-and-loop machinery every time, which is
	 * pure overhead when there's nothing to clamp. gfx_draw_char() runs
	 * once per character on every string this whole UI draws, so this
	 * adds up across a frame far more than a single call's cost suggests. */
	if (x >= 0 && y >= 0 && x + size <= screen_w && y + size <= screen_h) {
		for (int row = 0; row < 8; row++) {
			uint8_t bits = glyph[row];
			if (bits == 0) continue;
			for (int col = 0; col < 8; col++) {
				if (!(bits & (0x80 >> col))) continue;
				int px = x + col * glyph_scale, py = y + row * glyph_scale;
				for (int yy = 0; yy < glyph_scale; yy++) {
					gfx_color_t *dst = &back_buffer[(py + yy) * screen_w + px];
					for (int xx = 0; xx < glyph_scale; xx++) dst[xx] = fg;
				}
			}
		}
		return;
	}

	for (int row = 0; row < 8; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < 8; col++) {
			if (bits & (0x80 >> col)) {
				gfx_fill_rect(x + col * glyph_scale, y + row * glyph_scale, glyph_scale, glyph_scale, fg);
			}
		}
	}
}

void gfx_draw_char_bg(int x, int y, char c, gfx_color_t fg, gfx_color_t bg) {
	const uint8_t *glyph = font8x8_get_glyph(c);
	int size = 8 * glyph_scale;

	if (x >= 0 && y >= 0 && x + size <= screen_w && y + size <= screen_h) {
		for (int row = 0; row < 8; row++) {
			uint8_t bits = glyph[row];
			for (int col = 0; col < 8; col++) {
				gfx_color_t color = (bits & (0x80 >> col)) ? fg : bg;
				int px = x + col * glyph_scale, py = y + row * glyph_scale;
				for (int yy = 0; yy < glyph_scale; yy++) {
					gfx_color_t *dst = &back_buffer[(py + yy) * screen_w + px];
					for (int xx = 0; xx < glyph_scale; xx++) dst[xx] = color;
				}
			}
		}
		return;
	}

	for (int row = 0; row < 8; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < 8; col++) {
			gfx_color_t color = (bits & (0x80 >> col)) ? fg : bg;
			gfx_fill_rect(x + col * glyph_scale, y + row * glyph_scale, glyph_scale, glyph_scale, color);
		}
	}
}

int gfx_char_width(void) { return 8 * glyph_scale; }
int gfx_char_height(void) { return 8 * glyph_scale; }

void gfx_draw_string(int x, int y, const char *s, gfx_color_t fg) {
	int cx = x;
	int cw = gfx_char_width();
	int ch = gfx_char_height();
	while (*s) {
		if (*s == '\n') {
			cx = x;
			y += ch;
		} else {
			gfx_draw_char(cx, y, *s, fg);
			cx += cw;
		}
		s++;
	}
}

void gfx_draw_string_bg(int x, int y, const char *s, gfx_color_t fg, gfx_color_t bg) {
	int cx = x;
	int cw = gfx_char_width();
	int ch = gfx_char_height();
	while (*s) {
		if (*s == '\n') {
			cx = x;
			y += ch;
		} else {
			gfx_draw_char_bg(cx, y, *s, fg, bg);
			cx += cw;
		}
		s++;
	}
}

int gfx_string_width(const char *s) {
	int w = 0, max = 0, cw = gfx_char_width();
	while (*s) {
		if (*s == '\n') { if (w > max) max = w; w = 0; }
		else w += cw;
		s++;
	}
	return w > max ? w : max;
}

/* Nearest-neighbor scaled blit of a raw packed-RGB source image (3 bytes
 * per pixel, row-major, no padding) onto the back buffer. Used to draw
 * baked-in wallpapers at whatever the actual runtime resolution turns
 * out to be, since that isn't known at image-bake time. */
void gfx_blit_rgb_scaled(const unsigned char *src_rgb, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h) {
	if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) return;

	for (int y = 0; y < dst_h; y++) {
		int sy = (y * src_h) / dst_h;
		if (sy >= src_h) sy = src_h - 1;
		const unsigned char *src_row = &src_rgb[(size_t)sy * src_w * 3];
		int py = dst_y + y;
		if (py < 0 || py >= screen_h) continue;

		for (int x = 0; x < dst_w; x++) {
			int sx = (x * src_w) / dst_w;
			if (sx >= src_w) sx = src_w - 1;
			int px = dst_x + x;
			if (px < 0 || px >= screen_w) continue;

			const unsigned char *p = &src_row[sx * 3];
			back_buffer[py * screen_w + px] = GFX_RGB(p[0], p[1], p[2]);
		}
	}
}

void gfx_flip(void) {
	/* Fast path: the hardware's 32bpp layout already matches
	 * gfx_color_t's own format byte-for-byte, so there's nothing to
	 * unpack/repack per pixel - just copy the bytes across. This is
	 * the near-universal case (see fb_is_native_rgb888's declaration),
	 * and gfx_flip() runs unconditionally every frame regardless of
	 * whether anything on screen actually changed, so avoiding the
	 * per-pixel shift/mask work here is a real, broad win rather than
	 * a one-off optimization. A single memcpy covers the whole
	 * framebuffer when there's no row padding (pitch == width * 4);
	 * otherwise each row still gets one memcpy instead of per-pixel work. */
	if (fb_is_native_rgb888) {
		size_t row_bytes = (size_t)screen_w * 4;
		if (fb_pitch == row_bytes) {
			memcpy(fb_addr, back_buffer, row_bytes * (size_t)screen_h);
		} else {
			for (int y = 0; y < screen_h; y++) {
				memcpy(fb_addr + (size_t)y * fb_pitch, &back_buffer[y * screen_w], row_bytes);
			}
		}
		return;
	}

	for (int y = 0; y < screen_h; y++) {
		uint8_t *dst_row = fb_addr + (size_t)y * fb_pitch;
		gfx_color_t *src_row = &back_buffer[y * screen_w];
		if (fb_bpp == 32) {
			uint32_t *dst = (uint32_t *)dst_row;
			for (int x = 0; x < screen_w; x++) {
				gfx_color_t c = src_row[x];
				uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
				dst[x] = (r << fb_red_pos) | (g << fb_green_pos) | (b << fb_blue_pos);
			}
		} else {
			uint8_t *dst = dst_row;
			for (int x = 0; x < screen_w; x++) {
				gfx_color_t c = src_row[x];
				uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
				uint32_t packed = (r << fb_red_pos) | (g << fb_green_pos) | (b << fb_blue_pos);
				dst[x * 3 + 0] = packed & 0xFF;
				dst[x * 3 + 1] = (packed >> 8) & 0xFF;
				dst[x * 3 + 2] = (packed >> 16) & 0xFF;
			}
		}
	}
}
