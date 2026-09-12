# auroraOS rendering API reference

There are actually two rendering APIs in auroraOS, for two different
audiences:

1. **The C graphics library** (`kernel/gfx.c`) — used by kernel code
   itself (the console, the window manager, demo apps). Full API, called
   directly as C functions.
2. **The `SYS_GFX_*` syscalls** (`kernel/syscall.c`) — a small subset of
   (1), exposed to custom-compiled programs (see
   [ASSEMBLY.md](ASSEMBLY.md)) via `int 0x80`. Documented in full in
   [SYSCALLS.md](SYSCALLS.md); this document covers the underlying C API
   that those syscalls (and everything else on screen) are built on.

Both draw onto the same true-color linear framebuffer, set up once at boot
by negotiating a VBE graphics mode through GRUB (see the main
[README](../README.md) for how that boot sequence works) — there is no
legacy VGA text mode anywhere in this design.

## Core concepts

**Colors** are packed 0x00RRGGBB values (`gfx_color_t`, a `uint32_t`). The
`GFX_RGB(r, g, b)` macro builds one from three 0-255 components:

```c
gfx_color_t red = GFX_RGB(0xFF, 0x00, 0x00);
```

**Coordinates** are plain pixel `(x, y)` int pairs, origin `(0, 0)` at the
top-left, same as almost every 2D graphics API.

**Back buffer + flip**: every drawing function writes to an off-screen
buffer, not directly to the visible screen. Nothing appears until
`gfx_flip()` copies the back buffer to the real framebuffer. This exists
so a frame's worth of drawing (e.g. the window manager repainting the
whole desktop) appears all at once instead of visibly built up
stroke-by-stroke. Query the real screen size with `gfx_width()` /
`gfx_height()` rather than assuming a fixed resolution — see the note in
[SYSCALLS.md](SYSCALLS.md#sys_gfx_width-12--sys_gfx_height-13) on why.

## API reference (`kernel/gfx.c`, declared in `kernel/kernel.h`)

### Setup

```c
bool gfx_init(void);
```
Detects the VBE linear framebuffer GRUB set up (via the multiboot info
structure) and initializes the back buffer. Called once, early in boot
(`kernel_main()` in `kernel/kernel.c`). Returns `false` if no usable
framebuffer was found, in which case the kernel halts (there is no video
fallback).

```c
int gfx_width(void);
int gfx_height(void);
```
The actual screen dimensions in pixels.

### Pixels

```c
void gfx_putpixel(int x, int y, gfx_color_t color);
gfx_color_t gfx_getpixel(int x, int y);
void gfx_blend_pixel(int x, int y, gfx_color_t color, uint8_t alpha);
```
`gfx_blend_pixel` alpha-blends `color` over whatever's already there
(`alpha` 0 = no change, 255 = fully replaces it) — the basis for
transparency, soft shadows, and anti-aliasing elsewhere in this API.
Out-of-bounds coordinates are silently ignored (safe to call without
manual clipping).

### Rectangles

```c
void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);
void gfx_blend_rect(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha);
void gfx_draw_rect(int x, int y, int w, int h, gfx_color_t color);       /* outline only */
void gfx_draw_hline(int x, int y, int w, gfx_color_t color);
void gfx_draw_vline(int x, int y, int h, gfx_color_t color);
```

### Rounded rectangles & soft shadows

```c
void gfx_fill_round_rect(int x, int y, int w, int h, int radius, gfx_color_t color);
void gfx_blend_round_rect(int x, int y, int w, int h, int radius, gfx_color_t color, uint8_t alpha);
void gfx_draw_soft_shadow(int x, int y, int w, int h, int radius, int spread);
```
`gfx_draw_soft_shadow` draws several nested, decreasing-alpha rounded
rectangles outward from the given bounds — a cheap "fake gaussian blur"
that reads fine at desktop scale, used under every window in the GUI (see
`kernel/wm.c`).

### Lines

```c
void gfx_draw_line(int x0, int y0, int x1, int y1, gfx_color_t color);
```
Bresenham's algorithm; works for any slope/direction.

### Text

```c
void gfx_set_font_scale(int scale);
int gfx_char_width(void);   /* = 8 * scale */
int gfx_char_height(void);  /* = 8 * scale */
void gfx_draw_char(int x, int y, char c, gfx_color_t fg);
void gfx_draw_char_bg(int x, int y, char c, gfx_color_t fg, gfx_color_t bg);
void gfx_draw_string(int x, int y, const char *s, gfx_color_t fg);
void gfx_draw_string_bg(int x, int y, const char *s, gfx_color_t fg, gfx_color_t bg);
int gfx_string_width(const char *s);
```
Text is rendered from an 8x8 bitmap font (`kernel/font8x8.c`, covering
ASCII 0x20-0x7E) scaled up by `gfx_set_font_scale()` — the console uses
scale 1, the windowed desktop uses scale 2 for readability at typical
screen resolutions. `gfx_draw_string`/`gfx_string_width` handle embedded
`\n` as a line break advancing by `gfx_char_height()`. `_bg` variants also
fill the character's background cell, useful for a text cursor or
selection highlight without a separate rect fill underneath.

### Images

```c
void gfx_blit_rgb_scaled(const unsigned char *src_rgb, int src_w, int src_h,
                          int dst_x, int dst_y, int dst_w, int dst_h);
```
Nearest-neighbor scaled blit of a raw packed-RGB (3 bytes/pixel,
row-major) image onto the back buffer. This is how baked-in wallpapers
(`kernel/wallpaper.c`, images converted to C arrays at build time by
`tools/img_to_c.py` since there's no image decoder at runtime) get drawn
at whatever the real screen resolution turns out to be.

### Presenting a frame

```c
void gfx_flip(void);
```
Copies the back buffer to the real framebuffer, converting each pixel
from the internal 0xRRGGBB representation to the framebuffer's actual
pixel format (typically 32bpp XRGB, occasionally 24bpp — both are
handled; see `kernel/gfx.c`'s handling of the color channel positions
GRUB/the BIOS reported).

## Writing to the framebuffer from a custom program

Custom-compiled programs (see [ASSEMBLY.md](ASSEMBLY.md)) can't call these
C functions directly — they have no linkage to kernel code, and calling
into arbitrary kernel functions would bypass the one clean boundary this
design has. Instead they use the small `SYS_GFX_*` syscall subset
documented in [SYSCALLS.md](SYSCALLS.md), which covers pixels, rectangles,
lines, text, flipping, and querying the screen size — enough to draw a
simple scene, not the full rounded-rect/shadow/blit feature set kernel
code itself uses for the desktop GUI.
