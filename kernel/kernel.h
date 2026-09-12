#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint32_t gfx_color_t; /* 0x00RRGGBB */
#define GFX_RGB(r, g, b) (((gfx_color_t)(r) << 16) | ((gfx_color_t)(g) << 8) | (gfx_color_t)(b))

/* --- io.c --- */
uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t val);
uint16_t inw(uint16_t port);
void io_wait(void);

/* --- console.c: software text console rendered on the graphics
 * framebuffer (there is no separate hardware text mode once we boot
 * straight into a VBE linear framebuffer) --- */
void console_init(void);
void console_set_color(gfx_color_t color);
gfx_color_t console_color_default(void);
gfx_color_t console_color_accent(void);
gfx_color_t console_color_dim(void);
void console_clear(void);
void console_putchar(char c);
void console_write(const char *s, size_t len);
void console_writestring(const char *s);
void console_present(void);

/* --- string.c --- */
size_t strlen(const char *str);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strcat(char *dst, const char *src);
void *memset(void *dst, int val, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
char *strtok_simple(char *str, char delim, char **saveptr);

/* --- printf.c --- */
void kprintf(const char *fmt, ...);

/* --- gdt.c --- */
void gdt_install(void);

/* --- idt.c --- */
void idt_install(void);
/* Field order must exactly match what isr_common_stub/irq_common_stub in
 * boot.s push onto the stack (in reverse push order - the last thing
 * pushed sits at the lowest address, which is where %rsp/this struct's
 * first field points when the handler is entered), followed by int_no
 * and err_code, followed by what the CPU itself pushes on interrupt
 * entry (rip/cs/rflags/rsp/ss). */
struct registers {
	uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t int_no, err_code;
	uint64_t rip, cs, rflags, userrsp, ss;
};
typedef void (*isr_t)(struct registers *);
void register_interrupt_handler(uint8_t n, isr_t handler);

/* --- pic.c --- */
void pic_remap(void);
void pic_send_eoi(uint8_t irq);

/* --- timer.c --- */
void timer_install(void);
uint32_t timer_get_ticks(void);
void timer_wait(uint32_t ticks);

/* --- rtc.c --- */
struct rtc_time {
	uint8_t hours;   /* 0-23 */
	uint8_t minutes; /* 0-59 */
	uint8_t seconds; /* 0-59 */
};
void rtc_get_time(struct rtc_time *out);

/* --- keyboard.c --- */
void keyboard_install(void);
char keyboard_getchar_blocking(void);
bool keyboard_has_key(void);

/* Ctrl+<letter> comes through as byte values 1-26 (the same convention
 * terminals have always used: Ctrl+A=1 .. Ctrl+Z=26), so a GUI shortcut
 * bound to Ctrl+Q checks for CTRL_KEY('q') rather than the plain letter. */
#define CTRL_KEY(c) ((c) - 'a' + 1)

/* --- shell.c --- */
void shell_run(void);

/* --- memory.c --- */
struct fb_info {
	uint64_t addr;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint8_t bpp;
	uint8_t red_pos, red_size;
	uint8_t green_pos, green_size;
	uint8_t blue_pos, blue_size;
};
void memory_init(uint32_t mb_info_addr);
void memory_print_map(void);
uint32_t memory_total_kb(void);
bool memory_get_framebuffer(struct fb_info *out);

/* --- mouse.c --- */
void mouse_install(void);
void mouse_set_bounds(int w, int h);
void mouse_get_state(int *x, int *y, uint8_t *buttons);
bool mouse_poll_dirty(void);

/* --- font8x8.c --- */
const uint8_t *font8x8_get_glyph(char c);

/* --- gfx.c --- */
bool gfx_init(void);
int gfx_width(void);
int gfx_height(void);
void gfx_putpixel(int x, int y, gfx_color_t color);
gfx_color_t gfx_getpixel(int x, int y);
void gfx_blend_pixel(int x, int y, gfx_color_t color, uint8_t alpha);
void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t color);
void gfx_blend_rect(int x, int y, int w, int h, gfx_color_t color, uint8_t alpha);
void gfx_fill_round_rect(int x, int y, int w, int h, int radius, gfx_color_t color);
void gfx_blend_round_rect(int x, int y, int w, int h, int radius, gfx_color_t color, uint8_t alpha);
void gfx_draw_soft_shadow(int x, int y, int w, int h, int radius, int spread);
void gfx_draw_rect(int x, int y, int w, int h, gfx_color_t color);
void gfx_draw_hline(int x, int y, int w, gfx_color_t color);
void gfx_draw_vline(int x, int y, int h, gfx_color_t color);
void gfx_draw_line(int x0, int y0, int x1, int y1, gfx_color_t color);
void gfx_set_font_scale(int scale);
int gfx_char_width(void);
int gfx_char_height(void);
void gfx_draw_char(int x, int y, char c, gfx_color_t fg);
void gfx_draw_char_bg(int x, int y, char c, gfx_color_t fg, gfx_color_t bg);
void gfx_draw_string(int x, int y, const char *s, gfx_color_t fg);
void gfx_draw_string_bg(int x, int y, const char *s, gfx_color_t fg, gfx_color_t bg);
int gfx_string_width(const char *s);
void gfx_blit_rgb_scaled(const unsigned char *src_rgb, int src_w, int src_h, int dst_x, int dst_y, int dst_w, int dst_h);
void gfx_flip(void);

/* --- wallpaper.c: baked-in wallpaper images (see kernel/generated/,
 * produced from assets/wallpapers/ by tools/img_to_c.py) plus a small
 * registry so the desktop background can be swapped at runtime --- */
struct wallpaper {
	const char *name;
	const unsigned char *rgb;
	int width;
	int height;
};
int wallpaper_count(void);
const struct wallpaper *wallpaper_get(int index);
void wallpaper_draw(int index, int dst_w, int dst_h);

/* --- wm.c --- */
void wm_init(void);
void wm_run(void);

#endif
