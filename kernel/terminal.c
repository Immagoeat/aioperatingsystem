/* terminal.c - filesystem-aware shell commands: ls, cd, touch, rm, cat,
 * mkdir, echo (with file redirection), nano (a small full-screen text
 * editor), compile, and run. Delegated to from shell.c's dispatch().
 *
 * Directory nesting: exactly two levels are supported - the root
 * directory, and subdirectories directly under it (`cd DIRNAME` from
 * root, `cd ..` back to root). Deeper nesting isn't implemented; FAT16
 * itself supports arbitrary depth, but only this shallower shape has
 * actually been exercised/verified here, and claiming more than that
 * would be a real correctness risk for exactly the kind of mistake this
 * feature was asked not to have.
 */
#include "kernel.h"

#define MAX_PATH_NAME 32

/* 0 = root; non-zero = the first_cluster of the current subdirectory */
static uint16_t cwd_cluster = 0;
static char cwd_name[MAX_PATH_NAME] = ""; /* empty when at root */

static void local_strncpy_term(char *dst, const char *src, size_t n) {
	size_t i = 0;
	for (; i < n && src[i]; i++) dst[i] = src[i];
	dst[i] = '\0';
}

static bool fs_ready(void) {
	if (!fat16_is_mounted()) {
		console_set_color(GFX_RGB(0xFF, 0x6B, 0x6B));
		console_writestring("No filesystem available (no data disk was detected at boot).\n");
		console_set_color(console_color_default());
		return false;
	}
	return true;
}

void terminal_print_cwd_prompt_suffix(void) {
	if (cwd_name[0]) {
		console_putchar('/');
		console_writestring(cwd_name);
	}
}

/* --- ls --- */

struct ls_ctx {
	int count;
};

static void ls_print_entry(const char *name, bool is_dir, uint32_t size, void *userdata) {
	struct ls_ctx *ctx = (struct ls_ctx *)userdata;
	ctx->count++;
	if (is_dir) {
		console_set_color(GFX_RGB(0x5B, 0x9C, 0xFF));
		console_writestring(name);
		console_writestring("/\n");
		console_set_color(console_color_default());
	} else {
		console_writestring(name);
		kprintf("  (%u bytes)\n", size);
	}
}

static void cmd_ls(void) {
	if (!fs_ready()) return;
	struct ls_ctx ctx = { 0 };
	fat16_list(cwd_cluster, ls_print_entry, &ctx);
	if (ctx.count == 0) console_writestring("(empty)\n");
}

/* --- cd --- */

static void cmd_cd(const char *arg) {
	if (!fs_ready()) return;
	if (!arg || arg[0] == '\0') {
		cwd_cluster = 0;
		cwd_name[0] = '\0';
		return;
	}

	if (strcmp(arg, "..") == 0 || strcmp(arg, "/") == 0) {
		cwd_cluster = 0;
		cwd_name[0] = '\0';
		return;
	}

	if (cwd_cluster != 0) {
		console_writestring("Only one level of directories is supported; run 'cd ..' first.\n");
		return;
	}

	struct fat16_entry entry;
	if (!fat16_stat(0, arg, &entry)) {
		console_writestring("No such directory: ");
		console_writestring(arg);
		console_putchar('\n');
		return;
	}
	if (!entry.is_dir) {
		console_writestring(arg);
		console_writestring(" is not a directory.\n");
		return;
	}

	cwd_cluster = entry.first_cluster;
	local_strncpy_term(cwd_name, arg, sizeof(cwd_name) - 1);
}

/* --- mkdir / touch / rm --- */

static void cmd_mkdir(const char *arg) {
	if (!fs_ready()) return;
	if (!arg || arg[0] == '\0') { console_writestring("usage: mkdir NAME\n"); return; }
	if (cwd_cluster != 0) { console_writestring("mkdir only works from the root directory.\n"); return; }
	if (!fat16_create(cwd_cluster, arg, true)) {
		console_writestring("Could not create directory (disk full, or name too long for 8.3).\n");
	}
}

static void cmd_touch(const char *arg) {
	if (!fs_ready()) return;
	if (!arg || arg[0] == '\0') { console_writestring("usage: touch NAME\n"); return; }
	if (!fat16_create(cwd_cluster, arg, false)) {
		console_writestring("Could not create file (disk full, or name too long for 8.3).\n");
	}
}

static void cmd_rm(const char *arg) {
	if (!fs_ready()) return;
	if (!arg || arg[0] == '\0') { console_writestring("usage: rm NAME\n"); return; }
	if (!fat16_delete(cwd_cluster, arg)) {
		console_writestring("No such file: ");
		console_writestring(arg);
		console_putchar('\n');
	}
}

/* --- cat --- */

static void cmd_cat(const char *arg) {
	if (!fs_ready()) return;
	if (!arg || arg[0] == '\0') { console_writestring("usage: cat NAME\n"); return; }

	struct fat16_entry entry;
	if (!fat16_stat(cwd_cluster, arg, &entry) || entry.is_dir) {
		console_writestring("No such file: ");
		console_writestring(arg);
		console_putchar('\n');
		return;
	}

	static char buf[16384];
	uint32_t to_read = entry.size < sizeof(buf) - 1 ? entry.size : sizeof(buf) - 1;
	uint32_t got = fat16_read_file(entry.first_cluster, entry.size, 0, buf, to_read);
	buf[got] = '\0';
	console_writestring(buf);
	if (got > 0 && buf[got - 1] != '\n') console_putchar('\n');
}

/* --- echo with file redirection: echo TEXT > file.txt --- */

static void cmd_echo_maybe_redirect(char *rest) {
	if (!rest) { console_putchar('\n'); return; }

	char *redirect = rest;
	while (*redirect && !(redirect[0] == '>' )) redirect++;

	if (*redirect != '>') {
		console_writestring(rest);
		console_putchar('\n');
		return;
	}

	*redirect = '\0';
	char *filename = redirect + 1;
	while (*filename == ' ') filename++;

	/* trim trailing space on the text portion */
	size_t text_len = strlen(rest);
	while (text_len > 0 && rest[text_len - 1] == ' ') { rest[--text_len] = '\0'; }

	if (!fs_ready()) return;
	if (*filename == '\0') { console_writestring("usage: echo TEXT > filename\n"); return; }

	if (!fat16_write_file(cwd_cluster, filename, rest, (uint32_t)text_len)) {
		console_writestring("Could not write file.\n");
	}
}

/* --- nano: a small full-screen text editor ---
 *
 * Deliberately simple, matching what the keyboard driver actually
 * supports (no arrow keys - see keyboard.c): text entry is append/
 * backspace-only within the current line, Enter starts a new line,
 * Ctrl+S saves, Ctrl+X exits (prompting to save first if there are
 * unsaved changes). No cursor repositioning back into earlier lines -
 * a real constraint worth stating plainly rather than pretending this
 * is full nano. Good enough to write and revise a short assembly
 * program, which is its main purpose here. */
#define NANO_MAX_LINES 200
#define NANO_MAX_LINE_LEN 100

struct nano_buffer {
	char lines[NANO_MAX_LINES][NANO_MAX_LINE_LEN];
	int line_count;
	int cur_line;
	int cur_col;
	bool dirty;
};

static void nano_load(struct nano_buffer *nb, const char *filename) {
	memset(nb, 0, sizeof(*nb));
	nb->line_count = 1;

	struct fat16_entry entry;
	if (!fat16_stat(cwd_cluster, filename, &entry) || entry.is_dir) return;

	static char filebuf[32768];
	uint32_t to_read = entry.size < sizeof(filebuf) - 1 ? entry.size : sizeof(filebuf) - 1;
	uint32_t got = fat16_read_file(entry.first_cluster, entry.size, 0, filebuf, to_read);
	filebuf[got] = '\0';

	int line = 0, col = 0;
	for (uint32_t i = 0; i < got && line < NANO_MAX_LINES; i++) {
		if (filebuf[i] == '\n') {
			nb->lines[line][col] = '\0';
			line++;
			col = 0;
		} else if (col < NANO_MAX_LINE_LEN - 1) {
			nb->lines[line][col++] = filebuf[i];
		}
	}
	nb->lines[line][col] = '\0';
	nb->line_count = line + 1;
	nb->cur_line = line;
	nb->cur_col = col;
}

static bool nano_save(struct nano_buffer *nb, const char *filename) {
	static char out[32768];
	uint32_t pos = 0;
	for (int i = 0; i < nb->line_count && pos < sizeof(out) - 2; i++) {
		size_t len = strlen(nb->lines[i]);
		if (pos + len >= sizeof(out) - 2) len = sizeof(out) - 2 - pos;
		memcpy(out + pos, nb->lines[i], len);
		pos += (uint32_t)len;
		if (i < nb->line_count - 1) out[pos++] = '\n';
	}
	bool ok = fat16_write_file(cwd_cluster, filename, out, pos);
	if (ok) nb->dirty = false;
	return ok;
}

static void nano_redraw(struct nano_buffer *nb, const char *filename) {
	console_clear();
	console_set_color(console_color_accent());
	kprintf("-- nano: %s%s -- (Ctrl+S save, Ctrl+X exit)\n\n", filename, nb->dirty ? " [modified]" : "");
	console_set_color(console_color_default());

	int visible_start = 0;
	int max_visible = 30; /* keep well within the console's row count */
	if (nb->line_count > max_visible) visible_start = nb->line_count - max_visible;

	for (int i = visible_start; i < nb->line_count; i++) {
		console_writestring(nb->lines[i]);
		console_putchar('\n');
	}
}

static void cmd_nano(const char *filename) {
	if (!fs_ready()) return;
	if (!filename || filename[0] == '\0') { console_writestring("usage: nano FILENAME\n"); return; }

	static struct nano_buffer nb;
	nano_load(&nb, filename);
	nano_redraw(&nb, filename);
	console_present();

	for (;;) {
		char c = keyboard_getchar_blocking();

		if (c == CTRL_KEY('x')) {
			if (nb.dirty) {
				console_writestring("\nSave changes before exiting? (y/n) ");
				console_present();
				char answer = keyboard_getchar_blocking();
				console_putchar(answer);
				console_putchar('\n');
				console_present();
				if (answer == 'y' || answer == 'Y') {
					if (!nano_save(&nb, filename)) {
						console_writestring("Save failed; not exiting.\n");
						console_present();
						continue;
					}
				}
			}
			break;
		} else if (c == CTRL_KEY('s')) {
			if (!nano_save(&nb, filename)) {
				console_writestring("\nSave failed (disk full or name invalid).\n");
			}
			nano_redraw(&nb, filename);
			console_present();
		} else if (c == '\n') {
			if (nb.line_count < NANO_MAX_LINES) {
				nb.line_count++;
				nb.cur_line++;
				nb.cur_col = 0;
				nb.dirty = true;
				nano_redraw(&nb, filename);
				console_present();
			}
		} else if (c == '\b') {
			if (nb.cur_col > 0) {
				nb.cur_col--;
				nb.lines[nb.cur_line][nb.cur_col] = '\0';
				nb.dirty = true;
				nano_redraw(&nb, filename);
				console_present();
			} else if (nb.cur_line > 0) {
				/* merge with the previous line if it fits, matching how
				 * backspace-at-line-start normally behaves */
				int prev_len = (int)strlen(nb.lines[nb.cur_line - 1]);
				int this_len = (int)strlen(nb.lines[nb.cur_line]);
				if (prev_len + this_len < NANO_MAX_LINE_LEN) {
					strcat(nb.lines[nb.cur_line - 1], nb.lines[nb.cur_line]);
					for (int i = nb.cur_line; i < nb.line_count - 1; i++) {
						strcpy(nb.lines[i], nb.lines[i + 1]);
					}
					nb.line_count--;
					nb.cur_line--;
					nb.cur_col = prev_len;
					nb.dirty = true;
					nano_redraw(&nb, filename);
					console_present();
				}
			}
		} else if (c >= 32 && c < 127) {
			if (nb.cur_col < NANO_MAX_LINE_LEN - 1) {
				nb.lines[nb.cur_line][nb.cur_col++] = c;
				nb.lines[nb.cur_line][nb.cur_col] = '\0';
				nb.dirty = true;
				console_putchar(c);
				console_present();
			}
		}
	}

	console_clear();
	console_writestring("Exited nano.\n");
}

/* --- compile / run --- */

static void cmd_compile(const char *filename) {
	if (!fs_ready()) return;
	if (!filename || filename[0] == '\0') { console_writestring("usage: compile SOURCE.asm\n"); return; }

	struct fat16_entry entry;
	if (!fat16_stat(cwd_cluster, filename, &entry) || entry.is_dir) {
		console_writestring("No such file: ");
		console_writestring(filename);
		console_putchar('\n');
		return;
	}

	static char source[32768];
	uint32_t to_read = entry.size < sizeof(source) - 1 ? entry.size : sizeof(source) - 1;
	uint32_t got = fat16_read_file(entry.first_cluster, entry.size, 0, source, to_read);
	source[got] = '\0';

	static uint8_t code[65536];
	uint32_t code_len;
	char err[128];
	int err_line;

	if (!asm_assemble(source, code, sizeof(code), &code_len, err, &err_line)) {
		console_set_color(GFX_RGB(0xFF, 0x6B, 0x6B));
		kprintf("Compile error at line %d: %s\n", err_line, err);
		console_set_color(console_color_default());
		return;
	}

	/* output filename: same base name with a ".bin" extension, so
	 * "hello.asm" compiles to "hello.bin" - matching the 8.3 filename
	 * limit means the base name itself must already be <=8 chars. */
	char out_name[13];
	int i = 0;
	while (filename[i] && filename[i] != '.' && i < 8) { out_name[i] = filename[i]; i++; }
	out_name[i] = '\0';
	strcat(out_name, ".bin");

	if (!fat16_write_file(cwd_cluster, out_name, code, code_len)) {
		console_writestring("Compiled OK, but could not write output file.\n");
		return;
	}

	kprintf("Compiled %s -> %s (%u bytes)\n", filename, out_name, code_len);
}

static void cmd_run(const char *filename) {
	if (!fs_ready()) return;
	if (!filename || filename[0] == '\0') { console_writestring("usage: run PROGRAM.bin\n"); return; }

	struct fat16_entry entry;
	if (!fat16_stat(cwd_cluster, filename, &entry) || entry.is_dir) {
		console_writestring("No such file: ");
		console_writestring(filename);
		console_putchar('\n');
		return;
	}

	static uint8_t code[65536];
	uint32_t to_read = entry.size < sizeof(code) ? entry.size : sizeof(code);
	uint32_t got = fat16_read_file(entry.first_cluster, entry.size, 0, code, to_read);

	console_present(); /* flush any pending output before handing control to the program */
	uint64_t exit_code = asm_run(code, got);
	console_present();
	kprintf("\n[%s exited with code %llu]\n", filename, exit_code);
}

/* --- dispatch entry point, called from shell.c --- */

bool terminal_dispatch(const char *cmd, char *rest) {
	if (strcmp(cmd, "ls") == 0) { cmd_ls(); }
	else if (strcmp(cmd, "cd") == 0) { cmd_cd(rest); }
	else if (strcmp(cmd, "mkdir") == 0) { cmd_mkdir(rest); }
	else if (strcmp(cmd, "touch") == 0) { cmd_touch(rest); }
	else if (strcmp(cmd, "rm") == 0) { cmd_rm(rest); }
	else if (strcmp(cmd, "cat") == 0) { cmd_cat(rest); }
	else if (strcmp(cmd, "nano") == 0) { cmd_nano(rest); }
	else if (strcmp(cmd, "compile") == 0) { cmd_compile(rest); }
	else if (strcmp(cmd, "run") == 0) { cmd_run(rest); }
	else if (strcmp(cmd, "echo") == 0) { cmd_echo_maybe_redirect(rest); }
	else return false;
	return true;
}
