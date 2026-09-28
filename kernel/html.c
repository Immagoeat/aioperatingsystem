/* html.c - a real (if deliberately narrow) HTML renderer for the
 * browser app in wm.c. Not a layout engine in any serious sense:
 * there's no true positioning/box model, no images, no tables, no
 * variable font size (gfx.c only has one fixed-width bitmap font,
 * with a synthetic-bold variant - see gfx.c's file comment) - it
 * strips tags, decodes a handful of common entities, turns block-
 * level elements (<p>, <div>, <br>, <li>, headings) into line breaks,
 * and word-wraps the result to a caller-given column count. <script>
 * element contents are skipped entirely rather than rendered as
 * garbage text.
 *
 * Unlike the very first version of this file, <style> element content
 * (and inline style="" attributes) IS now applied, not skipped - via
 * css.c's real parser and cascade. Each output line is a sequence of
 * styled "runs" (see struct html_line/html_run in kernel.h) rather
 * than one plain string, since a single line can legitimately mix
 * styles (e.g. "plain text <strong>bold</strong> plain text" wrapped
 * onto one line has three runs). The pipeline is now three stages
 * instead of two: (1) collect every <style> block's text and parse it
 * into one cascade-ready sheet; (2) walk the HTML again, tracking
 * each element's resolved style (inherited color/bold/align, cascaded
 * with any tag/class/id rules and inline style="") and emit a flat
 * stream of styled "spans" (a word, or a paragraph-break marker) - see
 * struct html_span below; (3) word-wrap that span stream into
 * html_line/html_run records, splitting a long word across the wrap
 * width but never splitting one word's style. */
#include "kernel.h"

#define HTML_TAG_MAX 31
#define HTML_ATTR_VALUE_MAX 64
#define HTML_MAX_SPANS 8192

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

/* One word (or a paragraph-break marker, is_break true) plus the
 * resolved style it was produced under - the intermediate form
 * between the tag-walk (stage 2) and word-wrap (stage 3). A "word"
 * here never contains whitespace; runs of whitespace between words
 * are implicit (word-wrap re-inserts exactly one space between
 * consecutive non-break spans on the same line), matching real HTML's
 * own whitespace collapsing. */
struct html_span {
	char text[HTML_LINE_MAX];
	bool is_break;
	gfx_color_t color;
	gfx_color_t background;
	bool has_background;
	bool bold;
	int align;
};

/* Finds an attribute's value within a tag's raw attribute text (the
 * bytes between the tag name and the closing '>') - just enough
 * attribute parsing to pull out class="...", id="...", and
 * style="..." (the three attributes CSS selectors/inline styles in
 * this file's scope actually need). Handles both '"' and '\'' quoting
 * (real pages use both); an unquoted value (rare, but real: class=foo)
 * is read up to the next whitespace or '>'. Returns false if the
 * attribute isn't present at all. */
static bool html_find_attr(const uint8_t *html, uint32_t start, uint32_t end, const char *attr_name, char *out, uint32_t out_max) {
	uint32_t attr_len = (uint32_t)strlen(attr_name);
	uint32_t i = start;
	while (i < end) {
		while (i < end && html_isspace((char)html[i])) i++;
		uint32_t name_start = i;
		while (i < end && html[i] != '=' && !html_isspace((char)html[i])) i++;
		uint32_t name_len = i - name_start;

		bool is_match = (name_len == attr_len);
		if (is_match) {
			for (uint32_t k = 0; k < attr_len; k++) {
				char c = (char)html[name_start + k];
				if (c >= 'A' && c <= 'Z') c += 32;
				if (c != attr_name[k]) { is_match = false; break; }
			}
		}

		while (i < end && html_isspace((char)html[i])) i++;
		if (i >= end || html[i] != '=') {
			/* attribute with no value (e.g. a bare "disabled") - not
			 * what this function is looking for, move on */
			continue;
		}
		i++; /* consume '=' */
		while (i < end && html_isspace((char)html[i])) i++;

		char quote = 0;
		if (i < end && (html[i] == '"' || html[i] == '\'')) { quote = (char)html[i]; i++; }

		uint32_t val_start = i;
		if (quote) {
			while (i < end && html[i] != (uint8_t)quote) i++;
		} else {
			while (i < end && !html_isspace((char)html[i])) i++;
		}
		uint32_t val_len = i - val_start;

		if (is_match) {
			if (val_len > out_max - 1) val_len = out_max - 1;
			memcpy(out, html + val_start, val_len);
			out[val_len] = '\0';
			return true;
		}

		if (quote && i < end) i++; /* consume closing quote */
	}
	return false;
}

/* One entry in the element style stack the tag-walk maintains -
 * enough to resolve a child's inherited color/bold/align (CSS
 * properties this file supports are all inheritable in real CSS too)
 * and to know what to restore on the matching closing tag. Not a
 * full DOM: no tree, just a stack mirroring the nesting the tag-walk
 * is already doing. */
struct html_style_frame {
	gfx_color_t color;
	gfx_color_t background;
	bool has_background;
	bool bold;
	int align;
};
#define HTML_STYLE_STACK_MAX 64

/* --- stage 1: find every <style>...</style> block's raw text and
 * hand it to css.c - collected before the real tag-walk (stage 2) so
 * that a <style> block appearing after the content it styles (rare,
 * but real - some pages put it at the end of <body>) still applies,
 * matching how a real browser parses the whole document's stylesheets
 * before painting anything. */
static void html_collect_stylesheets(const uint8_t *html, uint32_t html_len, struct css_sheet *sheet) {
	uint32_t i = 0;
	while (i < html_len) {
		if (html[i] != '<') { i++; continue; }

		bool is_style_open = (i + 6 < html_len);
		if (is_style_open) {
			for (int k = 0; k < 6; k++) {
				char c = (char)html[i + 1 + k];
				if (c >= 'A' && c <= 'Z') c += 32;
				static const char want[] = "style";
				if (k < 5 && c != want[k]) { is_style_open = false; break; }
				if (k == 5 && c != '>' && !html_isspace(c)) { is_style_open = false; break; }
			}
		} else {
			is_style_open = false;
		}

		if (!is_style_open) { i++; continue; }

		uint32_t j = i;
		while (j < html_len && html[j] != '>') j++;
		if (j < html_len) j++; /* consume '>' */

		uint32_t content_start = j;
		while (j < html_len) {
			if (html[j] == '<' && j + 7 <= html_len) {
				bool match = true;
				static const char end_marker[] = "</style";
				for (int k = 0; k < 7; k++) {
					char c = (char)html[j + k];
					if (c >= 'A' && c <= 'Z') c += 32;
					if (c != end_marker[k]) { match = false; break; }
				}
				if (match) break;
			}
			j++;
		}
		uint32_t content_end = j;

		css_parse_stylesheet(html + content_start, content_end - content_start, sheet);

		while (j < html_len && html[j] != '>') j++;
		if (j < html_len) j++;
		i = j;
	}
}

/* Scans for <link rel="stylesheet" href="..."> tags and collects
 * their raw href values (no URL resolution - that needs the current
 * page's own URL, which this file doesn't know about; wm.c's
 * browser_load() resolves each one against the page URL and fetches
 * it before merging the result into the same cascade via
 * html_layout_with_css() below). This is what makes CSS visible on
 * real sites at all: almost none of them put their actual styling in
 * an inline <style> block - they link external .css files, which
 * this function is what discovers. rel="stylesheet" is checked
 * case-insensitively and only when it's the exact value (real pages
 * essentially never combine rel="stylesheet" with other rel values in
 * one attribute, unlike e.g. rel="alternate stylesheet" for print
 * stylesheets, which this deliberately does NOT match - only the
 * page's actual default stylesheet(s) are fetched). */
uint32_t html_find_stylesheet_links(const uint8_t *html, uint32_t html_len, char urls[][HTML_STYLESHEET_URL_MAX], uint32_t max_urls) {
	uint32_t count = 0;
	uint32_t i = 0;
	while (i < html_len && count < max_urls) {
		if (html[i] != '<') { i++; continue; }

		bool is_link = (i + 5 < html_len);
		if (is_link) {
			for (int k = 0; k < 5; k++) {
				char c = (char)html[i + 1 + k];
				if (c >= 'A' && c <= 'Z') c += 32;
				static const char want[] = "link ";
				if (c != want[k]) { is_link = false; break; }
			}
		} else {
			is_link = false;
		}
		if (!is_link) { i++; continue; }

		uint32_t attr_start = i + 6;
		uint32_t j = attr_start;
		while (j < html_len && html[j] != '>') j++;
		uint32_t attr_end = j;
		if (j < html_len) j++;

		char rel_buf[16] = {0};
		char href_buf[HTML_STYLESHEET_URL_MAX] = {0};
		bool has_rel = html_find_attr(html, attr_start, attr_end, "rel", rel_buf, sizeof(rel_buf));
		bool has_href = html_find_attr(html, attr_start, attr_end, "href", href_buf, sizeof(href_buf));

		bool rel_is_stylesheet = false;
		if (has_rel) {
			rel_is_stylesheet = (strlen(rel_buf) == 10);
			if (rel_is_stylesheet) {
				for (int k = 0; k < 10; k++) {
					char c = rel_buf[k];
					if (c >= 'A' && c <= 'Z') c += 32;
					static const char want[] = "stylesheet";
					if (c != want[k]) { rel_is_stylesheet = false; break; }
				}
			}
		}

		if (rel_is_stylesheet && has_href && href_buf[0] != '\0') {
			strcpy(urls[count++], href_buf);
		}

		i = j;
	}
	return count;
}

/* --- stage 2: walk the HTML, tracking each element's resolved style
 * on a stack, and emit a flat stream of styled spans (see struct
 * html_span above) into `spans`/`*span_count`. Block-level tags and
 * <br> each emit an is_break span; <script>/<style> element content
 * is skipped entirely (style content was already consumed in stage 1
 * - skipping it again here just means it's never rendered as text,
 * which is correct either way). */
static void html_walk(const uint8_t *html, uint32_t html_len, const struct css_sheet *sheet,
                       char *title, uint32_t title_max,
                       struct html_span *spans, uint32_t *span_count, uint32_t span_max) {
	uint32_t i = 0;
	bool in_title = false;
	uint32_t title_len = 0;

	struct html_style_frame stack[HTML_STYLE_STACK_MAX];
	int stack_top = 0;
	stack[0].color = GFX_RGB(0xEC, 0xEE, 0xF2); /* COL_TEXT - matches wm.c's default body text color, so unstyled pages look exactly like they did before CSS support existed */
	stack[0].background = 0;
	stack[0].has_background = false;
	stack[0].bold = false;
	stack[0].align = 0;

	char word[HTML_LINE_MAX];
	uint32_t word_len = 0;
	bool last_was_space = true;

	*span_count = 0;

	#define FLUSH_WORD() do { \
		if (word_len > 0 && *span_count < span_max) { \
			struct html_style_frame *cur = &stack[stack_top]; \
			struct html_span *sp = &spans[(*span_count)++]; \
			word[word_len] = '\0'; \
			strcpy(sp->text, word); \
			sp->is_break = false; \
			sp->color = cur->color; \
			sp->background = cur->background; \
			sp->has_background = cur->has_background; \
			sp->bold = cur->bold; \
			sp->align = cur->align; \
			word_len = 0; \
		} else { word_len = 0; } \
	} while (0)

	#define EMIT_BREAK() do { \
		FLUSH_WORD(); \
		if (*span_count < span_max) { \
			struct html_span *sp = &spans[(*span_count)++]; \
			sp->text[0] = '\0'; \
			sp->is_break = true; \
			sp->align = stack[stack_top].align; \
		} \
		last_was_space = true; \
	} while (0)

	while (i < html_len) {
		if (html[i] == '<') {
			char tag[HTML_TAG_MAX + 1];
			uint32_t tlen = 0;
			bool closing = false;
			uint32_t j = i + 1;
			if (j < html_len && html[j] == '/') { closing = true; j++; }
			uint32_t tag_name_start = j;
			while (j < html_len && html[j] != '>' && !html_isspace((char)html[j])) {
				if (tlen < HTML_TAG_MAX) tag[tlen++] = (char)(html[j] >= 'A' && html[j] <= 'Z' ? html[j] + 32 : html[j]);
				j++;
			}
			tag[tlen] = '\0';
			uint32_t attr_start = j;
			while (j < html_len && html[j] != '>') j++;
			uint32_t attr_end = j;
			if (j < html_len) j++; /* consume '>' */
			(void)tag_name_start;

			if (tag_is(tag, "script") || tag_is(tag, "style")) {
				if (!closing) {
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
			} else if (!closing && !tag_is(tag, "br") && stack_top < HTML_STYLE_STACK_MAX - 1) {
				/* Opening tag of a real (non-void) element: push a new
				 * style frame resolved from this element's tag/id/
				 * class/inline-style, inheriting whatever isn't
				 * overridden from the parent frame - real CSS
				 * inheritance for exactly the properties this file
				 * tracks. */
				char id_buf[HTML_ATTR_VALUE_MAX] = {0};
				char class_buf[HTML_ATTR_VALUE_MAX] = {0};
				char style_buf[HTML_ATTR_VALUE_MAX] = {0};
				bool has_id = html_find_attr(html, attr_start, attr_end, "id", id_buf, sizeof(id_buf));
				bool has_class = html_find_attr(html, attr_start, attr_end, "class", class_buf, sizeof(class_buf));
				bool has_style = html_find_attr(html, attr_start, attr_end, "style", style_buf, sizeof(style_buf));

				struct css_declared inline_decl;
				if (has_style) css_parse_inline_style(style_buf, &inline_decl);

				struct css_declared resolved;
				css_resolve_style(sheet, tag, has_id ? id_buf : NULL, has_class ? class_buf : NULL,
				                   has_style ? &inline_decl : NULL, &resolved);

				/* built-in tag defaults (heading/strong/em emphasis) -
				 * applied before the cascade result, so an explicit CSS
				 * rule can still override them, matching how a real
				 * browser's built-in UA stylesheet is the lowest-
				 * priority layer under any page CSS */
				struct html_style_frame frame = stack[stack_top];
				if (tag_is(tag, "strong") || tag_is(tag, "b") ||
				    tag_is(tag, "h1") || tag_is(tag, "h2") || tag_is(tag, "h3") ||
				    tag_is(tag, "h4") || tag_is(tag, "h5") || tag_is(tag, "h6")) {
					frame.bold = true;
				}

				if (resolved.has_color) frame.color = resolved.color;
				if (resolved.has_background) { frame.background = resolved.background; frame.has_background = true; }
				if (resolved.has_bold) frame.bold = resolved.bold;
				if (resolved.has_align) frame.align = resolved.align;

				stack_top++;
				stack[stack_top] = frame;
			} else if (closing && stack_top > 0) {
				FLUSH_WORD();
				stack_top--;
			}

			if (tag_is(tag, "br") || tag_is(tag, "p") || tag_is(tag, "div") ||
			    tag_is(tag, "li") || tag_is(tag, "tr") || tag_is(tag, "h1") ||
			    tag_is(tag, "h2") || tag_is(tag, "h3") || tag_is(tag, "h4") ||
			    tag_is(tag, "h5") || tag_is(tag, "h6")) {
				EMIT_BREAK();
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
			if (!last_was_space) { FLUSH_WORD(); last_was_space = true; }
		} else {
			if (word_len < HTML_LINE_MAX - 1) word[word_len++] = c;
			last_was_space = false;
		}
		i += advance;
	}
	FLUSH_WORD();

	title[title_len] = '\0';

	#undef FLUSH_WORD
	#undef EMIT_BREAK
}

/* --- stage 3: word-wrap the span stream into html_line/html_run
 * records. Consecutive same-style words on one line are merged into a
 * single run (so "the quick brown fox" in one color is one run, not
 * four) - a run boundary only appears where the style actually
 * changes or a line wraps, keeping paint_browser()'s per-line work
 * proportional to how many distinct styles a line actually has, not
 * how many words. */
void html_layout(const uint8_t *html_bytes, uint32_t html_len, struct html_page *page, int wrap_cols) {
	html_layout_with_css(html_bytes, html_len, NULL, 0, page, wrap_cols);
}

void html_layout_with_css(const uint8_t *html_bytes, uint32_t html_len, const uint8_t *extra_css, uint32_t extra_css_len, struct html_page *page, int wrap_cols) {
	if (wrap_cols < 8) wrap_cols = 8;
	if (wrap_cols > HTML_LINE_MAX - 1) wrap_cols = HTML_LINE_MAX - 1;

	static struct css_sheet sheet;
	memset(&sheet, 0, sizeof(sheet));
	/* External stylesheets are parsed first, then inline <style>
	 * blocks - matching real CSS source order (a <link> in <head>
	 * almost always precedes any inline <style>, and even when it
	 * doesn't, this is the same "later rules win ties" cascade
	 * ordering css_resolve_style() already implements, so getting this
	 * exactly byte-for-byte right relative to the real DOM order
	 * isn't necessary for correctness here - only relative order
	 * between rules of equal specificity matters, and inline
	 * style="" always wins regardless either way). */
	if (extra_css && extra_css_len > 0) css_parse_stylesheet(extra_css, extra_css_len, &sheet);
	html_collect_stylesheets(html_bytes, html_len, &sheet);

	static struct html_span spans[HTML_MAX_SPANS];
	uint32_t span_count = 0;
	html_walk(html_bytes, html_len, &sheet, page->title, sizeof(page->title), spans, &span_count, HTML_MAX_SPANS);

	page->line_count = 0;
	bool prev_blank = true;
	uint32_t i = 0;

	while (i < span_count && page->line_count < HTML_MAX_LINES - 1) {
		while (i < span_count && spans[i].is_break) i++; /* leading/collapsed breaks at the top of a line don't start a new blank line by themselves - handled via prev_blank below like the old version did */
		if (i >= span_count) break;

		struct html_line *line = &page->lines[page->line_count];
		line->run_count = 0;
		line->align = spans[i].align;
		int col = 0;
		bool any_word = false;

		while (i < span_count && !spans[i].is_break) {
			const struct html_span *sp = &spans[i];
			uint32_t word_len = (uint32_t)strlen(sp->text);
			if (word_len > (uint32_t)wrap_cols) word_len = (uint32_t)wrap_cols; /* one absurdly long "word" - hard-truncate rather than overflow the line */

			if (col > 0 && col + 1 + (int)word_len > wrap_cols) break; /* doesn't fit - picked up on the next line */

			bool need_space = (col > 0);
			int space_len = need_space ? 1 : 0;

			/* Append to the current run if this word's style matches
			 * it, otherwise start a new run. */
			bool can_extend = line->run_count > 0;
			if (can_extend) {
				struct html_run *last = &line->runs[line->run_count - 1];
				can_extend = last->color == sp->color && last->bold == sp->bold &&
				             last->has_background == sp->has_background &&
				             (!sp->has_background || last->background == sp->background);
				uint32_t cur_len = (uint32_t)strlen(last->text);
				can_extend = can_extend && (cur_len + (uint32_t)space_len + word_len < HTML_RUN_TEXT_MAX - 1);
			}

			if (can_extend) {
				struct html_run *last = &line->runs[line->run_count - 1];
				uint32_t cur_len = (uint32_t)strlen(last->text);
				if (need_space) last->text[cur_len++] = ' ';
				memcpy(last->text + cur_len, sp->text, word_len);
				last->text[cur_len + word_len] = '\0';
			} else if (line->run_count < HTML_MAX_RUNS_PER_LINE) {
				struct html_run *run = &line->runs[line->run_count++];
				uint32_t pos = 0;
				if (need_space) run->text[pos++] = ' ';
				/* A single word can be up to wrap_cols (HTML_LINE_MAX-1)
				 * bytes, but one run's buffer is deliberately much
				 * smaller (HTML_RUN_TEXT_MAX - see kernel.h's comment on
				 * why) - hard-truncate a word that alone would already
				 * overflow a fresh run, the same "truncate rather than
				 * overflow" choice already made for an oversized word
				 * against the line width above. Vanishingly rare in
				 * practice (this only bites a single unbroken run of 60+
				 * non-space characters), and truncating beats corrupting
				 * adjacent memory. */
				uint32_t copy_len = word_len;
				if (pos + copy_len > HTML_RUN_TEXT_MAX - 1) copy_len = HTML_RUN_TEXT_MAX - 1 - pos;
				memcpy(run->text + pos, sp->text, copy_len);
				run->text[pos + copy_len] = '\0';
				run->color = sp->color;
				run->background = sp->background;
				run->has_background = sp->has_background;
				run->bold = sp->bold;
			}
			/* if run_count has hit HTML_MAX_RUNS_PER_LINE, this word is
			 * silently dropped - an honest fixed-capacity limit like
			 * every other array in this codebase, not a crash */

			col += (int)word_len + space_len;
			any_word = true;
			i++;
		}

		bool this_blank = !any_word;
		if (!(this_blank && prev_blank)) {
			page->line_count++;
			prev_blank = this_blank;
		}

		if (i < span_count && spans[i].is_break) i++;
	}

	while (page->line_count > 0 && page->lines[page->line_count - 1].run_count == 0) page->line_count--;
}
