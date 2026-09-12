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

/* --- shell.c --- */
void shell_run(void);

/* --- memory.c --- */
void memory_init(uint32_t mb_info_addr);
void memory_print_map(void);
uint32_t memory_total_kb(void);

#endif
