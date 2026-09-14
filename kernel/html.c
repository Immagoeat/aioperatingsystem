/* html.c - a minimal, real (not simulated) HTML-to-text renderer for
 * the browser app in wm.c. This is not a layout engine in any serious
 * sense: there's no CSS, no images, no tables, no variable font sizes
 * (gfx.c only has one fixed-width bitmap font) - it strips tags,
 * decodes a handful of common entities, and turns block-level
 * elements (<p>, <div>, <br>, <li>, headings) into line breaks, then
 * word-wraps the result to a caller-given column count. <script> and
 * <style> element contents are skipped entirely rather than rendered
 * as garbage text, since that's the single most common way a naive
 * "just strip the tags" approach produces obviously-wrong output on
 * real pages. Good enough to actually read real, simple web pages -
 * not a Chromium replacement. */
#include "kernel.h"

#define HTML_TAG_MAX 31

static bool html_isspace(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool tag_is(const char *tag, const char *name) {
	return strcmp(tag, name) == 0;
}

/* Decodes the handful of entities real pages actually use. Anything
 * else (numeric entities beyond the common ones, rare named entities)
 * is passed through literally rather than guessed at - an honest
 * limitation, not a bug. */
static uint32_t decode_entity(const char *s, uint32_t len, char *out) {
	struct { const char *name; char ch; } table[] = {
		{ "amp;", '&' }, { "lt;", '<' }, { "gt;", '>' },
		{ "quot;", '"' }, { "#39;", '\'' }, { "apos;", '\'' },
		{ "nbsp;", ' ' },
	};
	for (uint32_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		uint32_t nlen = (uint32_t)strlen(table[i].name);
		if (len >= nlen) {
			bool match = true;
			for (uint32_t j = 0; j < nlen; j++) {
				if (s[j] != table[i].name[j]) { match = false; break; }
			}
			if (match) { *out = table[i].ch; return nlen; }
		}
	}
	*out = '&';
	return 0; /* unrecognized - caller just emits the literal '&' and moves on one byte */
}

/* --- stage 1: strip tags/entities/script-style content down to a
 * flat stream of words and explicit line-break markers ('\n' bytes),
 * written into `text` (caller-sized). Block-level tags and <br> each
 * emit a '\n'; runs of other whitespace collapse to a single ' ',
 * matching real HTML whitespace handling. */
static uint32_t html_strip_tags(const uint8_t *html, uint32_t html_len, char *title, uint32_t title_max, char *text, uint32_t text_max) {
	uint32_t i = 0;
	uint32_t out = 0;
	bool in_title = false;
	uint32_t title_len = 0;
	bool last_was_space = true; /* true so leading whitespace at the very start is dropped */

	while (i < html_len && out < text_max - 1) {
		if (html[i] == '<') {
			char tag[HTML_TAG_MAX + 1];
			uint32_t tlen = 0;
			bool closing = false;
			uint32_t j = i + 1;
			if (j < html_len && html[j] == '/') { closing = true; j++; }
			while (j < html_len && html[j] != '>' && html[j] != ' ' && html[j] != '\t' && html[j] != '\n' && !html_isspace((char)html[j])) {
				if (tlen < HTML_TAG_MAX) tag[tlen++] = (char)(html[j] >= 'A' && html[j] <= 'Z' ? html[j] + 32 : html[j]);
				j++;
			}
			tag[tlen] = '\0';
			/* skip to the matching '>' (handles attributes, including ones containing '>' inside quotes is not handled - real pages essentially never do this in practice) */
			while (j < html_len && html[j] != '>') j++;
			if (j < html_len) j++; /* consume '>' */

			if (tag_is(tag, "script") || tag_is(tag, "style")) {
				if (!closing) {
					/* skip everything up to the matching closing tag */
					const char *end_marker = tag_is(tag, "script") ? "</script" : "</style";
					uint32_t marker_len = (uint32_t)strlen(end_marker);
					while (j < html_len) {
						if (html[j] == '<' && j + marker_len <= html_len) {
							bool match = true;
							for (uint32_t k = 0; k < marker_len; k++) {
								char c1 = (char)html[j + k];
								if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
								char c2 = end_marker[k];
								if (c1 != c2) { match = false; break; }
							}
							if (match) break;
						}
						j++;
					}
					while (j < html_len && html[j] != '>') j++;
					if (j < html_len) j++;
				}
			} else if (tag_is(tag, "title")) {
				in_title = !closing;
			} else if (tag_is(tag, "br") || tag_is(tag, "p") || tag_is(tag, "div") ||
			           tag_is(tag, "li") || tag_is(tag, "tr") || tag_is(tag, "h1") ||
			           tag_is(tag, "h2") || tag_is(tag, "h3") || tag_is(tag, "h4") ||
			           tag_is(tag, "h5") || tag_is(tag, "h6")) {
				if (out > 0 && text[out - 1] != '\n') text[out++] = '\n';
				last_was_space = true;
			}
			i = j;
			continue;
		}

		char c;
		uint32_t advance = 1;
		if (html[i] == '&') {
			advance = decode_entity((const char *)html + i + 1, html_len - i - 1, &c);
			if (advance == 0) { c = '&'; advance = 1; } else { advance += 1; }
		} else {
			c = (char)html[i];
		}

		if (in_title) {
			if (title_len < title_max - 1 && c != '\n' && c != '\r' && c != '\t') title[title_len++] = c;
		} else if (html_isspace(c)) {
			if (!last_was_space && out < text_max - 1) { text[out++] = ' '; last_was_space = true; }
		} else {
			text[out++] = c;
			last_was_space = false;
		}
		i += advance;
	}

	title[title_len] = '\0';
	text[out] = '\0';
	return out;
}

/* --- stage 2: word-wrap the stripped text (which already has '\n'
 * block-break markers baked in) to `wrap_cols` columns, filling
 * `page` (already-allocated line array). Blank lines are collapsed
 * (no more than one blank line in a row) so <p><p><p> soup doesn't
 * turn into a wall of empty lines. */
void html_layout(const uint8_t *html_bytes, uint32_t html_len, struct html_page *page, int wrap_cols) {
	if (wrap_cols < 8) wrap_cols = 8;
	if (wrap_cols > HTML_LINE_MAX - 1) wrap_cols = HTML_LINE_MAX - 1;

	static char text[HTML_TEXT_MAX];
	uint32_t text_len = html_strip_tags(html_bytes, html_len, page->title, sizeof(page->title), text, sizeof(text));

	page->line_count = 0;
	bool prev_blank = true; /* true so leading blank lines are dropped */
	uint32_t i = 0;

	while (i < text_len && page->line_count < HTML_MAX_LINES - 1) {
		while (i < text_len && text[i] == ' ') i++;

		char *line = page->lines[page->line_count];
		int col = 0;
		bool wrote_anything = false;

		while (i < text_len && text[i] != '\n') {
			uint32_t word_start = i;
			uint32_t scan = i;
			while (scan < text_len && text[scan] != ' ' && text[scan] != '\n') scan++;
			uint32_t word_len = scan - word_start;

			if (word_len == 0) break;

			if (word_len > (uint32_t)wrap_cols) word_len = (uint32_t)wrap_cols; /* one absurdly long "word" (a URL, say) - hard-truncate rather than overflow the line */

			if (col > 0 && col + 1 + (int)word_len > wrap_cols) break; /* doesn't fit - stop this line without consuming it, so it's picked up on the next pass */

			i = scan; /* word (or its truncated prefix) fits - actually consume it */

			if (col > 0) { line[col++] = ' '; }
			memcpy(line + col, text + word_start, word_len);
			col += (int)word_len;
			wrote_anything = true;

			while (i < text_len && text[i] == ' ') i++;
		}

		line[col] = '\0';

		bool this_blank = (col == 0);
		if (!(this_blank && prev_blank)) {
			page->line_count++;
			prev_blank = this_blank;
		}
		(void)wrote_anything;

		if (i < text_len && text[i] == '\n') i++;
	}

	while (page->line_count > 0 && page->lines[page->line_count - 1][0] == '\0') page->line_count--;
}
