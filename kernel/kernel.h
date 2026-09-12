#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* --- io.c --- */
uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t val);
uint16_t inw(uint16_t port);
void io_wait(void);

/* --- vga.c / terminal --- */
enum vga_color {
	VGA_COLOR_BLACK = 0,
	VGA_COLOR_BLUE = 1,
	VGA_COLOR_GREEN = 2,
	VGA_COLOR_CYAN = 3,
	VGA_COLOR_RED = 4,
	VGA_COLOR_MAGENTA = 5,
	VGA_COLOR_BROWN = 6,
	VGA_COLOR_LIGHT_GREY = 7,
	VGA_COLOR_DARK_GREY = 8,
	VGA_COLOR_LIGHT_BLUE = 9,
	VGA_COLOR_LIGHT_GREEN = 10,
	VGA_COLOR_LIGHT_CYAN = 11,
	VGA_COLOR_LIGHT_RED = 12,
	VGA_COLOR_LIGHT_MAGENTA = 13,
	VGA_COLOR_LIGHT_BROWN = 14,
	VGA_COLOR_WHITE = 15,
};

void terminal_initialize(void);
void terminal_setcolor(uint8_t color);
void terminal_putchar(char c);
void terminal_write(const char *data, size_t size);
void terminal_writestring(const char *data);
void terminal_clear(void);
void terminal_backspace(void);
void terminal_set_cursor_visible(void);

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
struct registers {
	uint32_t ds;
	uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
	uint32_t int_no, err_code;
	uint32_t eip, cs, eflags, useresp, ss;
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

/* --- keyboard.c --- */
void keyboard_install(void);
char keyboard_getchar_blocking(void);
bool keyboard_has_key(void);

/* --- shell.c --- */
void shell_run(void);

/* --- memory.c --- */
void memory_init(uint32_t mb_info_addr);
void memory_print_map(void);
uint32_t memory_total_kb(void);

/* --- mouse.c --- */
void mouse_install(void);
void mouse_set_bounds(int w, int h);
void mouse_get_state(int *x, int *y, uint8_t *buttons);
bool mouse_poll_dirty(void);

/* --- vgamode13.c --- */
void vga_set_mode13h(void);
void vga_set_text_mode(void);
void vga_snapshot_current_mode(void);
void vga_set_palette_color(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/* --- font8x8.c --- */
const uint8_t *font8x8_get_glyph(char c);

/* --- gfx.c --- */
void gfx_init(void);
int gfx_width(void);
int gfx_height(void);
void gfx_putpixel(int x, int y, uint8_t color);
uint8_t gfx_getpixel(int x, int y);
void gfx_fill_rect(int x, int y, int w, int h, uint8_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint8_t color);
void gfx_draw_hline(int x, int y, int w, uint8_t color);
void gfx_draw_vline(int x, int y, int h, uint8_t color);
void gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color);
void gfx_draw_char(int x, int y, char c, uint8_t fg);
void gfx_draw_char_bg(int x, int y, char c, uint8_t fg, uint8_t bg);
void gfx_draw_string(int x, int y, const char *s, uint8_t fg);
void gfx_draw_string_bg(int x, int y, const char *s, uint8_t fg, uint8_t bg);
void gfx_flip(void);

/* --- wm.c --- */
void wm_init(void);
void wm_run(void);

#endif
