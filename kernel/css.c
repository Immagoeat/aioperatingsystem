/* css.c - a real (if deliberately narrow) CSS parser and cascade, for
 * the browser app in wm.c to actually apply page styling instead of
 * silently discarding it (see html.c's earlier file comment, which
 * used to state CSS was out of scope entirely - that's what this file
 * changes).
 *
 * Scope, stated plainly: properties actually renderable by this
 * kernel's one fixed-width bitmap font (gfx.c) - color,
 * background-color, font-weight (bold, via gfx_draw_string_bold()'s
 * synthetic dilation - see gfx.c's file comment), and text-align
 * (left/center/right). No font-size, no font-family, no italic, no
 * positioning/display/flexbox - this font physically cannot render
 * size or slant variation, and this isn't a layout engine. Selectors:
 * a single tag name, .class, or #id, and comma-separated groups of
 * those (e.g. "h1, .title, #main-heading") - no descendant/child
 * combinators, no pseudo-classes, no attribute selectors. Cascade:
 * specificity (id > class > tag, same weights CSS itself defines),
 * then source order for ties, then inline style="" always wins
 * (specificity infinity, matching real CSS) - a genuine, if minimal,
 * cascade rather than "last rule wins" or "first rule wins". */
#include "kernel.h"

/* struct css_declared/css_rule/css_sheet are defined in kernel.h (the
 * public API other files - html.c, wm.c - need to see their layout
 * too, not just call these functions), along with CSS_SHEET_MAX_RULES
 * and CSS_SELECTOR_NAME_MAX. selector_kind uses the same 0/1/2 =
 * tag/class/id values as the enum below. */
enum css_selector_kind { CSS_SEL_TAG = 0, CSS_SEL_CLASS = 1, CSS_SEL_ID = 2 };

#define CSS_VALUE_MAX 32

struct css_selector {
	enum css_selector_kind kind;
	char name[CSS_SELECTOR_NAME_MAX];
};

static bool css_isspace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static void css_skip_ws(const uint8_t *s, uint32_t len, uint32_t *pos) {
	while (*pos < len && css_isspace((char)s[*pos])) (*pos)++;
}

/* --- color parsing: #rgb, #rrggbb, rgb(r,g,b)/rgba(r,g,b,a) (alpha
 * ignored - this font has no transparency/blending concept in its
 * glyph rendering), and the common named colors real pages actually
 * use (not the full 148-name CSS list - an honest subset). */
struct css_named_color { const char *name; gfx_color_t color; };
static const struct css_named_color CSS_NAMED_COLORS[] = {
	{ "black", GFX_RGB(0x00,0x00,0x00) }, { "white", GFX_RGB(0xFF,0xFF,0xFF) },
	{ "red", GFX_RGB(0xFF,0x00,0x00) }, { "green", GFX_RGB(0x00,0x80,0x00) },
	{ "blue", GFX_RGB(0x00,0x00,0xFF) }, { "yellow", GFX_RGB(0xFF,0xFF,0x00) },
	{ "orange", GFX_RGB(0xFF,0xA5,0x00) }, { "purple", GFX_RGB(0x80,0x00,0x80) },
	{ "gray", GFX_RGB(0x80,0x80,0x80) }, { "grey", GFX_RGB(0x80,0x80,0x80) },
	{ "silver", GFX_RGB(0xC0,0xC0,0xC0) }, { "maroon", GFX_RGB(0x80,0x00,0x00) },
	{ "navy", GFX_RGB(0x00,0x00,0x80) }, { "teal", GFX_RGB(0x00,0x80,0x80) },
	{ "olive", GFX_RGB(0x80,0x80,0x00) }, { "lime", GFX_RGB(0x00,0xFF,0x00) },
	{ "aqua", GFX_RGB(0x00,0xFF,0xFF) }, { "cyan", GFX_RGB(0x00,0xFF,0xFF) },
	{ "magenta", GFX_RGB(0xFF,0x00,0xFF) }, { "fuchsia", GFX_RGB(0xFF,0x00,0xFF) },
	{ "pink", GFX_RGB(0xFF,0xC0,0xCB) }, { "brown", GFX_RGB(0xA5,0x2A,0x2A) },
	{ "transparent", GFX_RGB(0x00,0x00,0x00) },
};

static int css_hex_digit(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static bool css_parse_color(const char *value, gfx_color_t *out) {
	uint32_t len = (uint32_t)strlen(value);
	uint32_t i = 0;
	while (i < len && css_isspace(value[i])) i++;

	if (i < len && value[i] == '#') {
		i++;
		uint32_t start = i;
		while (i < len && css_hex_digit(value[i]) >= 0) i++;
		uint32_t hexlen = i - start;
		if (hexlen == 6) {
			int r = css_hex_digit(value[start]) * 16 + css_hex_digit(value[start+1]);
			int g = css_hex_digit(value[start+2]) * 16 + css_hex_digit(value[start+3]);
			int b = css_hex_digit(value[start+4]) * 16 + css_hex_digit(value[start+5]);
			*out = GFX_RGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
			return true;
		} else if (hexlen == 3) {
			int r = css_hex_digit(value[start]); r = r * 16 + r;
			int g = css_hex_digit(value[start+1]); g = g * 16 + g;
			int b = css_hex_digit(value[start+2]); b = b * 16 + b;
			*out = GFX_RGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
			return true;
		}
		return false;
	}

	if ((i + 4 <= len && value[i]=='r' && value[i+1]=='g' && value[i+2]=='b' && value[i+3]=='(') ||
	    (i + 5 <= len && value[i]=='r' && value[i+1]=='g' && value[i+2]=='b' && value[i+3]=='a' && value[i+4]=='(')) {
		while (i < len && value[i] != '(') i++;
		i++; /* consume '(' */
		int channel[3] = {0,0,0};
		for (int c = 0; c < 3; c++) {
			while (i < len && (css_isspace(value[i]) || value[i] == ',')) i++;
			int v = 0; bool any = false;
			while (i < len && value[i] >= '0' && value[i] <= '9') { v = v * 10 + (value[i]-'0'); i++; any = true; }
			if (!any) return false;
			if (v > 255) v = 255;
			channel[c] = v;
		}
		*out = GFX_RGB((uint8_t)channel[0], (uint8_t)channel[1], (uint8_t)channel[2]);
		return true;
	}

	/* named color: case-insensitive match against the table above */
	uint32_t name_len = len - i;
	for (uint32_t n = 0; n < sizeof(CSS_NAMED_COLORS)/sizeof(CSS_NAMED_COLORS[0]); n++) {
		const char *cand = CSS_NAMED_COLORS[n].name;
		uint32_t cand_len = (uint32_t)strlen(cand);
		if (cand_len != name_len) continue;
		bool match = true;
		for (uint32_t k = 0; k < cand_len; k++) {
			char a = value[i+k], b = cand[k];
			if (a >= 'A' && a <= 'Z') a += 32;
			if (a != b) { match = false; break; }
		}
		if (match) { *out = CSS_NAMED_COLORS[n].color; return true; }
	}
	return false;
}

/* Applies one property:value pair (already trimmed) to a declaration
 * set - shared by both <style> block rule bodies and inline style=""
 * parsing, since the property grammar is identical either way. */
static void css_apply_property(struct css_declared *decl, const char *prop, const char *value) {
	if (strcmp(prop, "color") == 0) {
		gfx_color_t c;
		if (css_parse_color(value, &c)) { decl->color = c; decl->has_color = true; }
	} else if (strcmp(prop, "background-color") == 0 || strcmp(prop, "background") == 0) {
		gfx_color_t c;
		if (css_parse_color(value, &c)) { decl->background = c; decl->has_background = true; }
	} else if (strcmp(prop, "font-weight") == 0) {
		bool bold = (strcmp(value, "bold") == 0) || (strcmp(value, "bolder") == 0);
		/* numeric weights: 700 and above is bold, per CSS's own definition */
		if (!bold && value[0] >= '0' && value[0] <= '9') {
			int w = 0; for (const char *p = value; *p >= '0' && *p <= '9'; p++) w = w * 10 + (*p - '0');
			bold = w >= 700;
		}
		decl->bold = bold;
		decl->has_bold = true;
	} else if (strcmp(prop, "text-align") == 0) {
		int align = -1;
		if (strcmp(value, "left") == 0) align = 0;
		else if (strcmp(value, "center") == 0) align = 1;
		else if (strcmp(value, "right") == 0) align = 2;
		if (align >= 0) { decl->align = align; decl->has_align = true; }
	}
	/* every other property (font-size, display, margin, ...) is
	 * recognized-but-unsupported or entirely unknown - silently
	 * ignored either way, the same as a real browser ignoring
	 * properties/values it doesn't understand rather than erroring */
}

/* Parses one CSS declaration block's contents (the part between { and
 * } - or, for inline style="", the whole attribute value) - a
 * sequence of "property: value;" pairs. */
static void css_parse_declarations(const uint8_t *s, uint32_t start, uint32_t end, struct css_declared *decl) {
	uint32_t i = start;
	while (i < end) {
		while (i < end && (css_isspace((char)s[i]) || s[i] == ';')) i++;
		if (i >= end) break;

		char prop[CSS_SELECTOR_NAME_MAX]; uint32_t plen = 0;
		while (i < end && s[i] != ':' && s[i] != ';' && plen < CSS_SELECTOR_NAME_MAX - 1) {
			char c = (char)s[i];
			if (!css_isspace(c)) prop[plen++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
			i++;
		}
		prop[plen] = '\0';
		while (i < end && s[i] != ':' && s[i] != ';') i++;
		if (i >= end || s[i] != ':') { while (i < end && s[i] != ';') i++; continue; }
		i++; /* consume ':' */
		while (i < end && css_isspace((char)s[i])) i++;

		char value[CSS_VALUE_MAX]; uint32_t vlen = 0;
		while (i < end && s[i] != ';' && vlen < CSS_VALUE_MAX - 1) { value[vlen++] = (char)s[i]; i++; }
		while (vlen > 0 && css_isspace(value[vlen-1])) vlen--;
		value[vlen] = '\0';

		if (plen > 0) css_apply_property(decl, prop, value);
	}
}

/* Parses one <style> block's full text into a sheet of rules -
 * "selector-group { declarations }" repeated, where selector-group is
 * comma-separated simple selectors (RFC-grammar-adjacent, not a full
 * CSS grammar - see this file's top comment for exactly what's
 * supported). Malformed input just stops adding rules rather than
 * erroring - a partially-broken stylesheet degrades to "some of the
 * page is styled, the rest falls back to defaults", never a crash. */
void css_parse_stylesheet(const uint8_t *css, uint32_t css_len, struct css_sheet *sheet) {
	uint32_t i = 0;
	while (i < css_len && sheet->rule_count < CSS_SHEET_MAX_RULES) {
		css_skip_ws(css, css_len, &i);
		if (i >= css_len) break;

		/* CSS comment (slash-star, star-slash) - skipped entirely,
		 * including any selector text inside it */
		if (i + 1 < css_len && css[i] == '/' && css[i+1] == '*') {
			i += 2;
			while (i + 1 < css_len && !(css[i] == '*' && css[i+1] == '/')) i++;
			i += 2;
			continue;
		}

		/* collect one comma-separated selector group up to '{' */
		struct css_selector selectors[16];
		int selector_count = 0;
		bool malformed = false;

		for (;;) {
			css_skip_ws(css, css_len, &i);
			if (i >= css_len) { malformed = true; break; }

			enum css_selector_kind kind = CSS_SEL_TAG;
			if (css[i] == '.') { kind = CSS_SEL_CLASS; i++; }
			else if (css[i] == '#') { kind = CSS_SEL_ID; i++; }
			else if (css[i] == '{' || css[i] == '}') { malformed = true; break; }

			char name[CSS_SELECTOR_NAME_MAX]; uint32_t nlen = 0;
			while (i < css_len && css[i] != ',' && css[i] != '{' && !css_isspace((char)css[i]) && nlen < CSS_SELECTOR_NAME_MAX - 1) {
				char c = (char)css[i];
				name[nlen++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
				i++;
			}
			name[nlen] = '\0';

			if (nlen > 0 && selector_count < 16) {
				selectors[selector_count].kind = kind;
				strcpy(selectors[selector_count].name, name);
				selector_count++;
			}

			css_skip_ws(css, css_len, &i);
			if (i < css_len && css[i] == ',') { i++; continue; }
			break;
		}

		if (malformed) {
			/* resync: skip to the next '}' (or end) and try again, so one
			 * bad rule doesn't stop every rule after it from parsing */
			while (i < css_len && css[i] != '}') i++;
			if (i < css_len) i++;
			continue;
		}

		css_skip_ws(css, css_len, &i);
		if (i >= css_len || css[i] != '{') {
			while (i < css_len && css[i] != '}') i++;
			if (i < css_len) i++;
			continue;
		}
		i++; /* consume '{' */
		uint32_t decl_start = i;
		while (i < css_len && css[i] != '}') i++;
		uint32_t decl_end = i;
		if (i < css_len) i++; /* consume '}' */

		struct css_declared decl;
		memset(&decl, 0, sizeof(decl));
		css_parse_declarations(css, decl_start, decl_end, &decl);

		for (int s = 0; s < selector_count && sheet->rule_count < CSS_SHEET_MAX_RULES; s++) {
			struct css_rule *rule = &sheet->rules[sheet->rule_count];
			rule->selector_kind = (int)selectors[s].kind;
			strcpy(rule->selector_name, selectors[s].name);
			rule->decl = decl;
			rule->source_order = sheet->rule_count;
			sheet->rule_count++;
		}
	}
}

/* Parses an inline style="..." attribute value directly into a
 * declaration set (no selector involved - it only ever applies to the
 * one element it's on). */
void css_parse_inline_style(const char *style_attr, struct css_declared *decl) {
	memset(decl, 0, sizeof(*decl));
	uint32_t len = (uint32_t)strlen(style_attr);
	css_parse_declarations((const uint8_t *)style_attr, 0, len, decl);
}

/* Specificity, per CSS's own definition restricted to the selector
 * kinds this file supports: id (100) > class (10) > tag (1). Real CSS
 * specificity is a 3-tuple compared lexicographically; collapsing it
 * to one integer is safe here specifically because no selector this
 * parser produces can combine kinds (each is exactly one simple
 * selector, never "div.foo#bar"), so the tuples never actually need
 * component-wise comparison - a plain integer comparison is
 * equivalent for this restricted grammar, not a shortcut that would
 * misrank rules a fuller CSS engine would rank differently. */
static int css_specificity(enum css_selector_kind kind) {
	switch (kind) {
		case CSS_SEL_ID: return 100;
		case CSS_SEL_CLASS: return 10;
		case CSS_SEL_TAG: default: return 1;
	}
}

/* Resolves the final style for one element by merging every rule in
 * `sheet` that matches (by tag name, and/or by one of its class
 * names, and/or by its id) property-by-property in specificity/source
 * order, then merging the element's own inline style (if any) last
 * with effectively-infinite specificity, matching real CSS's cascade
 * order exactly: stylesheet rules first (by specificity, then source
 * order), inline style always wins regardless of what any stylesheet
 * rule said. `classes` is a single space-separated string of class
 * names (as the class="..." attribute gives it), matched name-by-name
 * against each CSS_SEL_CLASS selector. */
void css_resolve_style(const struct css_sheet *sheet, const char *tag, const char *id, const char *classes,
                        const struct css_declared *inline_decl, struct css_declared *out) {
	memset(out, 0, sizeof(*out));

	int best_color_spec = -1, best_bg_spec = -1, best_bold_spec = -1, best_align_spec = -1;
	int best_color_order = -1, best_bg_order = -1, best_bold_order = -1, best_align_order = -1;

	for (int i = 0; i < sheet->rule_count; i++) {
		const struct css_rule *rule = &sheet->rules[i];
		bool matches = false;

		if (rule->selector_kind == CSS_SEL_TAG) {
			matches = tag && strcmp(rule->selector_name, tag) == 0;
		} else if (rule->selector_kind == CSS_SEL_ID) {
			matches = id && strcmp(rule->selector_name, id) == 0;
		} else { /* CSS_SEL_CLASS */
			if (classes) {
				const char *p = classes;
				while (*p) {
					while (*p == ' ') p++;
					const char *word_start = p;
					while (*p && *p != ' ') p++;
					uint32_t wlen = (uint32_t)(p - word_start);
					uint32_t slen = (uint32_t)strlen(rule->selector_name);
					if (wlen == slen && wlen > 0 && strncmp(word_start, rule->selector_name, wlen) == 0) { matches = true; break; }
				}
			}
		}
		if (!matches) continue;

		int spec = css_specificity(rule->selector_kind);
		if (rule->decl.has_color && (spec > best_color_spec || (spec == best_color_spec && rule->source_order > best_color_order))) {
			out->color = rule->decl.color; out->has_color = true; best_color_spec = spec; best_color_order = rule->source_order;
		}
		if (rule->decl.has_background && (spec > best_bg_spec || (spec == best_bg_spec && rule->source_order > best_bg_order))) {
			out->background = rule->decl.background; out->has_background = true; best_bg_spec = spec; best_bg_order = rule->source_order;
		}
		if (rule->decl.has_bold && (spec > best_bold_spec || (spec == best_bold_spec && rule->source_order > best_bold_order))) {
			out->bold = rule->decl.bold; out->has_bold = true; best_bold_spec = spec; best_bold_order = rule->source_order;
		}
		if (rule->decl.has_align && (spec > best_align_spec || (spec == best_align_spec && rule->source_order > best_align_order))) {
			out->align = rule->decl.align; out->has_align = true; best_align_spec = spec; best_align_order = rule->source_order;
		}
	}

	if (inline_decl) {
		if (inline_decl->has_color) { out->color = inline_decl->color; out->has_color = true; }
		if (inline_decl->has_background) { out->background = inline_decl->background; out->has_background = true; }
		if (inline_decl->has_bold) { out->bold = inline_decl->bold; out->has_bold = true; }
		if (inline_decl->has_align) { out->align = inline_decl->align; out->has_align = true; }
	}
}
