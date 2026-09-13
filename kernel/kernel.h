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
void outw(uint16_t port, uint16_t val);
void io_wait(void);

/* --- ata.c: polling PIO ATA disk driver (primary bus, master drive) --- */
bool ata_init(void);
bool ata_is_present(void);
uint32_t ata_get_total_sectors(void);
bool ata_read_sector(uint32_t lba, uint8_t *buf512);
bool ata_write_sector(uint32_t lba, const uint8_t *buf512);

/* --- asm.c: assembler + runner for auroraOS's custom instruction set
 * (see docs/ASSEMBLY.md). asm_assemble() encodes `source` into machine
 * code in out_code (capacity out_capacity), returning the encoded
 * length via out_len; on failure returns false and fills out_error /
 * out_error_line. asm_run() executes an already-assembled buffer
 * directly - no isolation from the kernel, see asm.c's file comment. */
bool asm_assemble(const char *source, uint8_t *out_code, uint32_t out_capacity, uint32_t *out_len, char *out_error, int *out_error_line);
uint64_t asm_run(const uint8_t *code, uint32_t len);
extern uint64_t asm_last_exit_code; /* set by SYS_EXIT; see syscall.c */

/* --- fat16.c: FAT16 filesystem driver --- */
struct fat16_entry {
	char name[13]; /* "NAME.EXT\0" */
	bool is_dir;
	uint32_t size;
	uint16_t first_cluster;
};
typedef void (*fat16_list_callback)(const char *name, bool is_dir, uint32_t size, void *userdata);

bool fat16_mount(void);
bool fat16_is_mounted(void);
bool fat16_format(uint32_t disk_sectors);
void fat16_list(uint16_t dir_cluster, fat16_list_callback cb, void *userdata);
bool fat16_stat(uint16_t dir_cluster, const char *name, struct fat16_entry *out);
bool fat16_create(uint16_t dir_cluster, const char *name, bool as_directory);
bool fat16_delete(uint16_t dir_cluster, const char *name);
uint32_t fat16_read_file(uint16_t first_cluster, uint32_t file_size, uint32_t offset, void *out, uint32_t max_len);
bool fat16_write_file(uint16_t dir_cluster, const char *name, const void *data, uint32_t len);
#define FAT16_ROOT_CLUSTER 0

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
void console_set_cursor(int x, int y);

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

/* --- syscall.c: the syscall ABI custom-compiled programs use to call
 * into the kernel (see docs/SYSCALLS.md for the full reference). Number
 * goes in rax, up to 4 args in rdi/rsi/rdx/r10, return value in rax,
 * invoked via `int 0x80`. --- */
#define SYS_EXIT         0
#define SYS_WRITE_CHAR   1
#define SYS_WRITE_STR    2
#define SYS_WRITE_INT    3
#define SYS_READ_CHAR    4
#define SYS_HAS_KEY      5
#define SYS_GFX_SET_EXTRA 6
#define SYS_GFX_PIXEL    7
#define SYS_GFX_RECT     8
#define SYS_GFX_LINE     9
#define SYS_GFX_TEXT     10
#define SYS_GFX_FLIP     11
#define SYS_GFX_WIDTH    12
#define SYS_GFX_HEIGHT   13
#define SYS_GET_TICKS    14
#define SYS_SLEEP_TICKS  15
void syscall_dispatch(struct registers *regs);

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

/* Arrow keys are pushed into the same char-based keyboard buffer as
 * everything else, using values above the ASCII/Ctrl-code range (which
 * only ever produces 0, 1-26, or 32-126) so callers can tell them apart
 * from any real character with a plain equality check - no separate
 * "is this an arrow key" API needed. `char` is signed on this platform,
 * so these are written as explicit negative values instead of raw
 * bytes >= 128, which would be surprising/UB-adjacent to write as a
 * plain int-to-char narrowing conversion. */
#define KEY_ARROW_UP    ((char)-1)
#define KEY_ARROW_DOWN  ((char)-2)
#define KEY_ARROW_LEFT  ((char)-3)
#define KEY_ARROW_RIGHT ((char)-4)

/* --- shell.c --- */
void shell_run(void);
/* Launches the shell as a GUI app (see wm.c's "Terminal"): same
 * commands as the top-level shell, but with `exit` in place of `gui`.
 * Caller is responsible for switching graphics modes before/after,
 * same as the shell (Ctrl+Q) and Text Editor already do. */
void terminal_app_run(void);

/* --- terminal.c: filesystem-aware commands (ls, cd, touch, rm, cat,
 * mkdir, echo with redirection, nano, compile, run). Returns false if
 * `cmd` isn't one it handles, so shell.c's dispatch() can fall through
 * to its own commands / the "unknown command" message. */
bool terminal_dispatch(const char *cmd, char *rest);
void terminal_print_cwd_prompt_suffix(void);
/* Launches nano as a full-screen console app from the GUI (see wm.c's
 * "Text Editor" app): prompts for a filename, then edits it. Caller is
 * responsible for switching graphics modes before/after, same as the
 * shell (Ctrl+Q) already does. */
void terminal_launch_nano_from_gui(void);

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
