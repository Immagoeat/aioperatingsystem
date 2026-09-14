/* noteedit.c - the text-buffer editing core shared by both of nano's
 * renderers: the full-screen console version (terminal.c's `nano`
 * command) and the windowed version (wm.c's "Text Editor" app).
 *
 * Deliberately has zero rendering calls in it (no console_*, no
 * gfx_*) - every function here only reads/mutates a struct
 * note_buffer. This is what makes it possible to have one real set of
 * editing semantics (insert/delete at cursor, line splitting, arrow
 * movement with clamping/wrapping) instead of two copies that could
 * quietly drift apart. Both renderers call note_handle_key() and then
 * draw whatever the resulting note_buffer looks like, however they
 * each know how to draw it. */
#include "kernel.h"

void note_load(struct note_buffer *nb, uint16_t dir_cluster, const char *filename) {
	memset(nb, 0, sizeof(*nb));
	nb->line_count = 1;

	struct fat16_entry entry;
	if (!fat16_stat(dir_cluster, filename, &entry) || entry.is_dir) return;

	static char filebuf[32768];
	uint32_t to_read = entry.size < sizeof(filebuf) - 1 ? entry.size : sizeof(filebuf) - 1;
	uint32_t got = fat16_read_file(entry.first_cluster, entry.size, 0, filebuf, to_read);
	filebuf[got] = '\0';

	int line = 0, col = 0;
	for (uint32_t i = 0; i < got && line < NOTE_MAX_LINES; i++) {
		if (filebuf[i] == '\n') {
			nb->lines[line][col] = '\0';
			line++;
			col = 0;
		} else if (col < NOTE_MAX_LINE_LEN - 1) {
			nb->lines[line][col++] = filebuf[i];
		}
	}
	nb->lines[line][col] = '\0';
	nb->line_count = line + 1;
	nb->cur_line = line;
	nb->cur_col = col;
}

bool note_save(struct note_buffer *nb, uint16_t dir_cluster, const char *filename) {
	static char out[32768];
	uint32_t pos = 0;
	for (int i = 0; i < nb->line_count && pos < sizeof(out) - 2; i++) {
		size_t len = strlen(nb->lines[i]);
		if (pos + len >= sizeof(out) - 2) len = sizeof(out) - 2 - pos;
		memcpy(out + pos, nb->lines[i], len);
		pos += (uint32_t)len;
		if (i < nb->line_count - 1) out[pos++] = '\n';
	}
	bool ok = fat16_write_file(dir_cluster, filename, out, pos);
	if (ok) nb->dirty = false;
	return ok;
}

/* Width (in characters) of the "NNN | " line-number gutter, sized to
 * the line count so numbers stay right-aligned and the "|" separator
 * lines up even once the file grows past 9/99/999 lines. Both
 * renderers draw a gutter, so this lives here rather than being
 * duplicated in each. */
int note_line_number_digits(const struct note_buffer *nb) {
	int digits = 1;
	for (int n = nb->line_count; n >= 10; n /= 10) digits++;
	return digits;
}

/* Handles one keystroke: Enter (split line), Backspace (delete before
 * cursor / merge with previous line), the four arrow keys (move with
 * clamping/wrapping), or a printable character (insert at cursor).
 * Ctrl+S/Ctrl+X and anything else app-chrome-related (save prompts,
 * exiting) are left to the caller, since "what does exiting look
 * like" is a rendering/UI question each renderer answers differently.
 * Returns true if the buffer changed and needs to be redrawn. */
bool note_handle_key(struct note_buffer *nb, char c) {
	if (c == '\n') {
		if (nb->line_count >= NOTE_MAX_LINES) return false;
		char *cur = nb->lines[nb->cur_line];
		for (int i = nb->line_count; i > nb->cur_line + 1; i--) {
			strcpy(nb->lines[i], nb->lines[i - 1]);
		}
		strcpy(nb->lines[nb->cur_line + 1], cur + nb->cur_col);
		cur[nb->cur_col] = '\0';
		nb->line_count++;
		nb->cur_line++;
		nb->cur_col = 0;
		nb->dirty = true;
		return true;
	}

	if (c == '\b') {
		if (nb->cur_col > 0) {
			char *line = nb->lines[nb->cur_line];
			int len = (int)strlen(line);
			for (int i = nb->cur_col - 1; i < len; i++) {
				line[i] = line[i + 1];
			}
			nb->cur_col--;
			nb->dirty = true;
			return true;
		}
		if (nb->cur_line > 0) {
			int prev_len = (int)strlen(nb->lines[nb->cur_line - 1]);
			int this_len = (int)strlen(nb->lines[nb->cur_line]);
			if (prev_len + this_len < NOTE_MAX_LINE_LEN) {
				strcat(nb->lines[nb->cur_line - 1], nb->lines[nb->cur_line]);
				for (int i = nb->cur_line; i < nb->line_count - 1; i++) {
					strcpy(nb->lines[i], nb->lines[i + 1]);
				}
				nb->line_count--;
				nb->cur_line--;
				nb->cur_col = prev_len;
				nb->dirty = true;
				return true;
			}
		}
		return false;
	}

	if (c == KEY_ARROW_LEFT) {
		if (nb->cur_col > 0) {
			nb->cur_col--;
		} else if (nb->cur_line > 0) {
			nb->cur_line--;
			nb->cur_col = (int)strlen(nb->lines[nb->cur_line]);
		}
		return true;
	}

	if (c == KEY_ARROW_RIGHT) {
		int len = (int)strlen(nb->lines[nb->cur_line]);
		if (nb->cur_col < len) {
			nb->cur_col++;
		} else if (nb->cur_line < nb->line_count - 1) {
			nb->cur_line++;
			nb->cur_col = 0;
		}
		return true;
	}

	if (c == KEY_ARROW_UP) {
		if (nb->cur_line > 0) {
			nb->cur_line--;
			int len = (int)strlen(nb->lines[nb->cur_line]);
			if (nb->cur_col > len) nb->cur_col = len;
		}
		return true;
	}

	if (c == KEY_ARROW_DOWN) {
		if (nb->cur_line < nb->line_count - 1) {
			nb->cur_line++;
			int len = (int)strlen(nb->lines[nb->cur_line]);
			if (nb->cur_col > len) nb->cur_col = len;
		}
		return true;
	}

	if (c >= 32 && c < 127) {
		char *line = nb->lines[nb->cur_line];
		int len = (int)strlen(line);
		if (len < NOTE_MAX_LINE_LEN - 1) {
			for (int i = len; i >= nb->cur_col; i--) {
				line[i + 1] = line[i];
			}
			line[nb->cur_col] = c;
			nb->cur_col++;
			nb->dirty = true;
			return true;
		}
		return false;
	}

	return false;
}
