/**
 * @file markdown.c
 * @brief Minimal markdown parser/renderer producing pre-wrapped, ANSI
 *        styled terminal lines (see include/markdown.h).
 *
 * Design: a line-oriented block pass (fences, headings, lists, quotes,
 * rules, paragraphs) drives an inline pass (**bold**, *italic*, `code`,
 * [text](url)) that emits runs of text tagged with a style bitmask. The
 * styled text is then wrapped to the target width: SGR sequences and
 * UTF-8 continuation bytes never count toward the column budget and are
 * never split; the active style is closed at each line break and reopened
 * on the continuation line.
 */

#include "markdown.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Style bits (internal). */
#define MD_B_BOLD   0x01u
#define MD_B_DIM    0x02u
#define MD_B_ITALIC 0x04u
#define MD_B_UNDER  0x08u
#define MD_B_CODE   0x10u /* magenta fg */
#define MD_B_CYAN   0x40u

#define SGR_MAX 48 /* max bytes of one SGR sequence we emit */

/* ------------------------------------------------------------------ */
/* Style <-> SGR helpers                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Writes the full SGR sequence for a style mask into buf.
 *
 * Every sequence starts with reset (0) so it fully re-specifies the
 * style, which keeps reopening after line wraps trivial.
 *
 * @param buf Destination buffer (at least SGR_MAX bytes).
 * @param mask Style bits.
 * @return Number of bytes written (excluding the NUL terminator).
 */
static size_t md_sgr_for_mask(unsigned mask, char *buf)
{
	static const struct {
		unsigned    bit;
		const char *code;
	} table[] = {
		{ MD_B_BOLD,   ";1"   },
		{ MD_B_DIM,    ";2"   },
		{ MD_B_ITALIC, ";3"   },
		{ MD_B_UNDER,  ";4"   },
		{ MD_B_CODE,   ";35"  },
		{ MD_B_CYAN,   ";36"  },
	};

	memcpy(buf, "\033[0", 3);
	size_t off = 3;
	if (mask != 0) {
		for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
			if ((mask & table[i].bit) == 0) {
				continue;
			}
			size_t clen = strlen(table[i].code);
			memcpy(buf + off, table[i].code, clen);
			off += clen;
		}
	}
	buf[off++] = 'm';
	buf[off]   = '\0';
	return off;
}

/**
 * @brief Decodes one of our own SGR sequences back into a style mask.
 *
 * Only understands the sequences produced by md_sgr_for_mask() (always
 * starting with reset); returns 0 for anything else.
 */
static unsigned md_sgr_mask(const char *sgr, size_t len)
{
	if (len < 4 || sgr[0] != '\033' || sgr[1] != '[' || sgr[2] != '0') {
		return 0;
	}
	unsigned mask = 0;
	size_t   i    = 3;
	while (i < len && sgr[i] != 'm') {
		size_t start = i;
		while (i < len && sgr[i] != ';' && sgr[i] != 'm') {
			i++;
		}
		size_t plen = i - start;
		if (plen == 1 && sgr[start] == '1') mask |= MD_B_BOLD;
		else if (plen == 1 && sgr[start] == '2') mask |= MD_B_DIM;
		else if (plen == 1 && sgr[start] == '3') mask |= MD_B_ITALIC;
		else if (plen == 1 && sgr[start] == '4') mask |= MD_B_UNDER;
		else if (plen == 2 && sgr[start] == '3' && sgr[start + 1] == '5') mask |= MD_B_CODE;
		else if (plen == 2 && sgr[start] == '3' && sgr[start + 1] == '6') mask |= MD_B_CYAN;
		if (i < len && sgr[i] == ';') {
			i++;
		}
	}
	return mask;
}

/* ------------------------------------------------------------------ */
/* Display width (SGR- and UTF-8-continuation-aware)                  */
/* ------------------------------------------------------------------ */

static size_t md_disp_width(const char *s, size_t len)
{
	size_t w = 0;
	for (size_t i = 0; i < len;) {
		if (s[i] == '\033') {
			/* Skip a whole escape sequence up to 'm'. */
			size_t j = i + 1;
			if (j < len && s[j] == '[') {
				j++;
			}
			while (j < len && s[j] != 'm') {
				j++;
			}
			i = (j < len) ? j + 1 : j;
			continue;
		}
		if ((((unsigned char)s[i]) & 0xC0) != 0x80) {
			w++;
		}
		i++;
	}
	return w;
}

/* ------------------------------------------------------------------ */
/* Growable string buffer                                             */
/* ------------------------------------------------------------------ */

typedef struct {
	char    *data;
	size_t   len;
	size_t   cap;
	unsigned mask; /* currently open style */
} md_sbuf_t;

static void md_sb_init(md_sbuf_t *sb)
{
	sb->data = NULL;
	sb->len  = 0;
	sb->cap  = 0;
	sb->mask = 0;
}

static void md_sb_free(md_sbuf_t *sb)
{
	free(sb->data);
	md_sb_init(sb);
}

/** Ensures room for extra more bytes; 0 on success, -1 on failure. */
static int md_sb_reserve(md_sbuf_t *sb, size_t extra)
{
	if (sb->len + extra + 1 <= sb->cap) {
		return 0;
	}
	size_t cap = sb->cap ? sb->cap : 64;
	while (cap < sb->len + extra + 1) {
		cap *= 2;
	}
	char *grown = realloc(sb->data, cap);
	if (!grown) {
		return -1;
	}
	sb->data = grown;
	sb->cap  = cap;
	return 0;
}

/** Appends raw bytes verbatim (used for internal SGR sequences). */
static int md_sb_raw(md_sbuf_t *sb, const char *s, size_t n)
{
	if (n == 0) {
		return 0;
	}
	if (md_sb_reserve(sb, n) != 0) {
		return -1;
	}
	memcpy(sb->data + sb->len, s, n);
	sb->len += n;
	return 0;
}

/**
 * @brief Appends user content: switches style if needed and filters
 *        control characters (terminal escape-injection safety).
 */
static int md_sb_text(md_sbuf_t *sb, const char *s, size_t n, unsigned mask)
{
	if (sb->mask != mask) {
		char   sgr[SGR_MAX];
		size_t slen = md_sgr_for_mask(mask, sgr);
		if (md_sb_raw(sb, sgr, slen) != 0) {
			return -1;
		}
		sb->mask = mask;
	}
	char tmp[2];
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\t') {
			c = ' ';
		} else if (c < 0x20 || c == 0x7f) {
			continue; /* drop other control bytes */
		}
		tmp[0] = (char)c;
		if (md_sb_raw(sb, tmp, 1) != 0) {
			return -1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Line array                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
	md_line_t *items;
	size_t     count;
	size_t     cap;
} md_larr_t;

/**
 * @brief Pushes an owned text string as the next line.
 *
 * On success ownership of text moves into the array; on failure text is
 * freed and -1 is returned.
 */
static int md_larr_push_text(md_larr_t *la, char *text)
{
	if (la->count >= la->cap) {
		size_t     cap    = la->cap ? la->cap * 2 : 16;
		md_line_t *grown = realloc(la->items, cap * sizeof(md_line_t));
		if (!grown) {
			free(text);
			return -1;
		}
		la->items = grown;
		la->cap   = cap;
	}
	la->items[la->count].text = text;
	la->count++;
	return 0;
}

static void md_larr_free(md_larr_t *la)
{
	for (size_t i = 0; i < la->count; i++) {
		free(la->items[i].text);
	}
	free(la->items);
	md_larr_t empty = { NULL, 0, 0 };
	*la             = empty;
}

/* ------------------------------------------------------------------ */
/* Render context                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
	md_larr_t lines;
	int       width; /* clamped wrapping width */
	int       failed;
} md_ctx_t;

/** Pushes an empty (blank) output line. */
static void md_ctx_push_blank(md_ctx_t *ctx)
{
	char *text = malloc(1);
	if (!text) {
		ctx->failed = 1;
		return;
	}
	text[0] = '\0';
	if (md_larr_push_text(&ctx->lines, text) != 0) {
		ctx->failed = 1;
	}
}

/* ------------------------------------------------------------------ */
/* Inline markdown parsing                                            */
/* ------------------------------------------------------------------ */

static int md_parse_inline(md_ctx_t *ctx, const char *s, size_t len,
			   unsigned mask, md_sbuf_t *out);

static size_t md_find_char(const char *s, size_t len, size_t from, char c)
{
	for (size_t i = from; i < len; i++) {
		if (s[i] == c) {
			return i;
		}
	}
	return (size_t)-1;
}

static size_t md_find_str2(const char *s, size_t len, size_t from,
			   const char *two)
{
	for (size_t i = from; i + 1 < len; i++) {
		if (s[i] == two[0] && s[i + 1] == two[1]) {
			return i;
		}
	}
	return (size_t)-1;
}

static size_t md_find_str3(const char *s, size_t len, size_t from,
			   const char *three)
{
	for (size_t i = from; i + 2 < len; i++) {
		if (s[i] == three[0] && s[i + 1] == three[1] &&
		    s[i + 2] == three[2]) {
			return i;
		}
	}
	return (size_t)-1;
}

/**
 * @brief Parses inline markdown into styled runs appended to out.
 *
 * Unclosed delimiters are emitted literally so that streaming output
 * (partially received markdown) degrades gracefully.
 */
static int md_parse_inline(md_ctx_t *ctx, const char *s, size_t len,
			   unsigned mask, md_sbuf_t *out)
{
	(void)ctx;
	size_t i = 0;
	while (i < len) {
		char c = s[i];

		/* Inline code: `code` (content verbatim). */
		if (c == '`') {
			size_t close = md_find_char(s, len, i + 1, '`');
			if (close != (size_t)-1 && close > i + 1) {
				if (md_sb_text(out, s + i + 1, close - i - 1,
					       mask | MD_B_CODE) != 0) {
					return -1;
				}
				i = close + 1;
				continue;
			}
		}

		/* Bold+italic: ***text*** */
		if (c == '*' && i + 2 < len && s[i + 1] == '*' &&
		    s[i + 2] == '*') {
			size_t close = md_find_str3(s, len, i + 3, "***");
			if (close != (size_t)-1) {
				if (md_parse_inline(ctx, s + i + 3,
						    close - (i + 3),
						    mask | MD_B_BOLD | MD_B_ITALIC,
						    out) != 0) {
					return -1;
				}
				i = close + 3;
				continue;
			}
		}

		/* Bold: **text** */
		if (c == '*' && i + 1 < len && s[i + 1] == '*') {
			size_t close = md_find_str2(s, len, i + 2, "**");
			if (close != (size_t)-1) {
				/* If the content holds exactly one unmatched
				 * single '*' and the closer is itself followed
				 * by another '*', the closing pair overlaps an
				 * inner italic closer (**x *y***): shift by one
				 * so the inner style composes. */
				size_t singles = 0;
				for (size_t k = i + 2; k < close; k++) {
					if (s[k] == '*' && k + 1 < close &&
					    s[k + 1] == '*') {
						k++;
						continue;
					}
					if (s[k] == '*') {
						singles++;
					}
				}
				if (singles == 1 && close + 2 < len &&
				    s[close + 2] == '*') {
					close++;
				}
				if (md_parse_inline(ctx, s + i + 2,
						    close - (i + 2),
						    mask | MD_B_BOLD, out) != 0) {
					return -1;
				}
				i = close + 2;
				continue;
			}
		}

		/* Italic: *text* or _text_ ('_' only outside words). */
		if (c == '*' || c == '_') {
			int ok = 1;
			if (c == '_') {
				/* No intraword emphasis for underscores. */
				ok = (i == 0) || !(((s[i - 1] >= 'a') && (s[i - 1] <= 'z')) ||
						   ((s[i - 1] >= 'A') && (s[i - 1] <= 'Z')) ||
						   ((s[i - 1] >= '0') && (s[i - 1] <= '9')));
			}
			if (ok) {
				size_t close = md_find_char(s, len, i + 1, c);
				if (close != (size_t)-1 && close > i + 1) {
					if (md_parse_inline(ctx, s + i + 1,
							    close - i - 1,
							    mask | MD_B_ITALIC,
							    out) != 0) {
						return -1;
					}
					i = close + 1;
					continue;
				}
			}
		}

		/* Link: [text](url) -> underlined text + dim " (url)". */
		if (c == '[') {
			size_t close = md_find_char(s, len, i + 1, ']');
			if (close != (size_t)-1 && close + 1 < len &&
			    s[close + 1] == '(') {
				size_t paren = md_find_char(s, len, close + 2, ')');
				if (paren != (size_t)-1 && paren > close + 2) {
					if (md_parse_inline(ctx, s + i + 1,
							    close - i - 1,
							    mask | MD_B_UNDER,
							    out) != 0) {
						return -1;
					}
					if (md_sb_text(out, " (", 2,
						       mask | MD_B_DIM) != 0 ||
					    md_sb_text(out, s + close + 2,
						       paren - close - 2,
						       mask | MD_B_DIM) != 0 ||
					    md_sb_text(out, ")", 1,
						       mask | MD_B_DIM) != 0) {
						return -1;
					}
					i = paren + 1;
					continue;
				}
			}
		}

		/* Literal character. */
		if (md_sb_text(out, s + i, 1, mask) != 0) {
			return -1;
		}
		i++;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Wrapping                                                           */
/* ------------------------------------------------------------------ */

/**
 * @brief Extracts the style active at the end of a wrapped segment.
 *
 * Scans the segment for SGR sequences (the last one wins); if the
 * segment contains none, the inherited style from the previous line is
 * kept. Returns 1 when a style is active at segment end.
 */
static int md_seg_final_style(const char *seg, size_t len,
			      const char *inh, size_t inh_len,
			      char *out, size_t *out_len)
{
	size_t last = (size_t)-1;
	for (size_t i = 0; i < len; i++) {
		if (seg[i] == '\033') {
			last = i;
		}
	}
	if (last != (size_t)-1) {
		size_t j = last + 1;
		if (j < len && seg[j] == '[') {
			j++;
		}
		while (j < len && seg[j] != 'm') {
			j++;
		}
		size_t slen = (j < len) ? (j - last + 1) : (len - last);
		if (slen > SGR_MAX - 1) {
			slen = SGR_MAX - 1;
		}
		memcpy(out, seg + last, slen);
		*out_len = slen;
		return 1;
	}
	if (inh_len > 0) {
		memcpy(out, inh, inh_len);
		*out_len = inh_len;
		return 1;
	}
	*out_len = 0;
	return 0;
}

/**
 * @brief Pushes one wrapped segment as an output line, managing style
 *        reopening (inherited style prepended, reset appended).
 */
static int md_push_segment(md_ctx_t *ctx, const char *seg, size_t len,
			   const char *prefix, const char *inh, size_t inh_len)
{
	char   final_sgr[SGR_MAX];
	size_t final_len = 0;
	int    has_style = md_seg_final_style(seg, len, inh, inh_len,
					      final_sgr, &final_len);
	unsigned final_mask = has_style ? md_sgr_mask(final_sgr, final_len) : 0;
	unsigned inh_mask   = (inh_len > 0) ? md_sgr_mask(inh, inh_len) : 0;

	md_sbuf_t sb;
	md_sb_init(&sb);
	if (inh_mask != 0 && md_sb_raw(&sb, inh, inh_len) != 0) {
		md_sb_free(&sb);
		ctx->failed = 1;
		return -1;
	}
	if (md_sb_raw(&sb, seg, len) != 0 ||
	    (final_mask != 0 && md_sb_raw(&sb, "\033[0m", 4) != 0)) {
		md_sb_free(&sb);
		ctx->failed = 1;
		return -1;
	}
	size_t plen = prefix ? strlen(prefix) : 0;
	char *full = malloc(plen + sb.len + 1);
	if (!full) {
		md_sb_free(&sb);
		ctx->failed = 1;
		return -1;
	}
	if (plen > 0) {
		memcpy(full, prefix, plen);
	}
	if (sb.len > 0) {
		memcpy(full + plen, sb.data, sb.len);
	}
	full[plen + sb.len] = '\0';
	md_sb_free(&sb);

	if (md_larr_push_text(&ctx->lines, full) != 0) {
		ctx->failed = 1;
		return -1;
	}
	return 0;
}

/**
 * @brief Wraps styled text into output lines of at most width columns.
 *
 * SGR sequences and UTF-8 continuation bytes never count toward the
 * width. When word_wrap is set, breaks at the last space; otherwise
 * hard-breaks (code blocks). The active style is closed at each break
 * and reopened on the continuation line.
 *
 * @param first_prefix Prefix for the first produced line (may be NULL).
 * @param rest_prefix  Prefix for continuation lines (may be NULL).
 * @return Number of lines pushed (via lines->count delta by caller).
 */
static void md_wrap_styled(md_ctx_t *ctx, const char *s, size_t len,
			   int width, int word_wrap,
			   const char *first_prefix, const char *rest_prefix)
{
	char   inh[SGR_MAX];
	size_t inh_len = 0;

	size_t line_start = 0;
	int    col        = 0;
	long   last_space = -1;

	size_t   i    = 0;
	size_t   emitted = 0;
	while (i < len) {
		if (s[i] == '\033') {
			size_t j = i + 1;
			if (j < len && s[j] == '[') {
				j++;
			}
			while (j < len && s[j] != 'm') {
				j++;
			}
			if (j < len) {
				j++;
			}
			i = j;
			continue;
		}
		unsigned char b = (unsigned char)s[i];
		if ((b & 0xC0) != 0x80) {
			/* New display column starts here. */
			if (col >= width) {
				size_t brk;
				size_t next;
				if (word_wrap && last_space >= 0 &&
				    (size_t)last_space > line_start) {
					brk  = (size_t)last_space;
					next = brk + 1;
				} else {
					brk  = i;
					next = i;
				}
				if (word_wrap) {
					while (next < len && s[next] == ' ') {
						next++;
					}
				}
				/* Trim trailing spaces of the segment. */
				size_t end = brk;
				while (end > line_start && s[end - 1] == ' ') {
					end--;
				}
				if (end > line_start) {
					const char *prefix =
						(emitted == 0) ? first_prefix
							       : rest_prefix;
					if (md_push_segment(ctx, s + line_start,
							    end - line_start,
							    prefix, inh,
							    inh_len) != 0) {
						return;
					}
					emitted++;
				}
				/* Style active at the break reopens on the
				 * next line; recompute from the segment. */
				char   fin[SGR_MAX];
				size_t fin_len = 0;
				size_t seg_len = brk - line_start;
				if (md_seg_final_style(s + line_start, seg_len,
						       inh, inh_len, fin,
						       &fin_len) &&
				    md_sgr_mask(fin, fin_len) != 0) {
					memcpy(inh, fin, fin_len);
					inh_len = fin_len;
				} else {
					inh_len = 0;
				}
				line_start = next;
				col        = 0;
				last_space = -1;
				/* Resume scanning at the new line start so already-
				 * passed characters are recounted into col (word
				 * break) or the break char starts the new line
				 * (hard break). */
				i = next;
				continue;
			}
			if (s[i] == ' ') {
				last_space = (long)i;
			}
			col++;
		}
		i++;
	}

	/* Final segment. */
	size_t end = len;
	while (end > line_start && s[end - 1] == ' ') {
		end--;
	}
	if (end > line_start) {
		const char *prefix = (emitted == 0) ? first_prefix : rest_prefix;
		if (md_push_segment(ctx, s + line_start, end - line_start,
				    prefix, inh, inh_len) != 0) {
			return;
		}
		emitted++;
	}
}

/* ------------------------------------------------------------------ */
/* Block rendering                                                    */
/* ------------------------------------------------------------------ */

/**
 * @brief Renders one logical line: inline-parses it, wraps it to the
 *        content width and pushes prefixed output lines.
 *
 * Used for paragraphs, headings, list items and quotes. When nothing is
 * produced (empty content) a bare prefix line is pushed so structural
 * prefixes (e.g. quote bars) still render.
 */
static void md_render_prefixed(md_ctx_t *ctx, const char *s, size_t len,
			       unsigned mask, const char *first_prefix,
			       const char *rest_prefix)
{
	size_t before = ctx->lines.count;
	if (len == 0) {
		/* Nothing to wrap: keep the structural prefix visible. */
		const char *pfx = (first_prefix && first_prefix[0]) ? first_prefix : "";
		char *text = malloc(strlen(pfx) + 1);
		if (!text) {
			ctx->failed = 1;
			return;
		}
		strcpy(text, pfx);
		if (md_larr_push_text(&ctx->lines, text) != 0) {
			ctx->failed = 1;
		}
		return;
	}

	md_sbuf_t styled;
	md_sb_init(&styled);
	if (md_parse_inline(ctx, s, len, mask, &styled) != 0) {
		md_sb_free(&styled);
		ctx->failed = 1;
		return;
	}

	size_t pw1 = first_prefix ? md_disp_width(first_prefix, strlen(first_prefix)) : 0;
	size_t pw2 = rest_prefix ? md_disp_width(rest_prefix, strlen(rest_prefix)) : 0;
	size_t pmax = (pw1 > pw2) ? pw1 : pw2;
	int    cw   = ctx->width - (int)pmax;
	if (cw < 1) {
		cw = 1;
	}

	md_wrap_styled(ctx, styled.data, styled.len, cw, 1,
		       first_prefix, rest_prefix);
	md_sb_free(&styled);

	if (ctx->lines.count == before) {
		/* Styled to nothing (e.g. only filtered bytes): emit the
		 * prefix so structure is still visible. */
		md_ctx_push_blank(ctx);
	}
}

static void md_render_code_line(md_ctx_t *ctx, const char *s, size_t len)
{
	md_sbuf_t styled;
	md_sb_init(&styled);
	if (md_sb_text(&styled, s, len, 0) != 0) {
		md_sb_free(&styled);
		ctx->failed = 1;
		return;
	}
	int cw = ctx->width - 2;
	if (cw < 1) {
		cw = 1;
	}
	md_wrap_styled(ctx, styled.data, styled.len, cw, 0, "  ", "  ");
	md_sb_free(&styled);
	if (ctx->lines.count == 0) {
		md_ctx_push_blank(ctx);
	}
}

static void md_render_hr(md_ctx_t *ctx)
{
	md_sbuf_t sb;
	md_sb_init(&sb);
	char   sgr[SGR_MAX];
	size_t slen = md_sgr_for_mask(MD_B_DIM, sgr);
	if (md_sb_raw(&sb, sgr, slen) != 0) {
		md_sb_free(&sb);
		ctx->failed = 1;
		return;
	}
	for (int i = 0; i < ctx->width; i++) {
		if (md_sb_raw(&sb, "\xe2\x94\x80", 3) != 0) {
			md_sb_free(&sb);
			ctx->failed = 1;
			return;
		}
	}
	if (md_sb_raw(&sb, "\033[0m", 4) != 0) {
		md_sb_free(&sb);
		ctx->failed = 1;
		return;
	}
	if (md_sb_reserve(&sb, 1) != 0) {
		md_sb_free(&sb);
		ctx->failed = 1;
		return;
	}
	sb.data[sb.len] = '\0';
	if (md_larr_push_text(&ctx->lines, sb.data) != 0) {
		ctx->failed = 1;
	}
}

/* ------------------------------------------------------------------ */
/* Block detection helpers                                            */
/* ------------------------------------------------------------------ */

static int md_line_blank(const char *s, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (s[i] != ' ' && s[i] != '\t') {
			return 0;
		}
	}
	return 1;
}

/**
 * @brief Detects a code fence line.
 *
 * @param fence_char Receives the fence character ('`' or '~').
 * @param fence_len  Receives the fence run length.
 * @param info_off   Receives the offset of the info string (after the
 *                   fence run, trimmed); may be NULL.
 * @param info_len   Receives the info string length; may be NULL.
 * @return 1 when the line opens (or closes) a fence.
 */
static int md_is_fence(const char *s, size_t len, char *fence_char,
		       size_t *fence_len, size_t *info_off, size_t *info_len)
{
	if (len < 3 || (s[0] != '`' && s[0] != '~')) {
		return 0;
	}
	size_t run = 1;
	while (run < len && s[run] == s[0]) {
		run++;
	}
	if (run < 3) {
		return 0;
	}
	size_t off = run;
	while (off < len && s[off] == ' ') {
		off++;
	}
	size_t ilen = (off < len) ? len - off : 0;
	/* Trailing spaces on the info string. */
	while (ilen > 0 && (s[off + ilen - 1] == ' ' || s[off + ilen - 1] == '\t')) {
		ilen--;
	}
	if (fence_char) {
		*fence_char = s[0];
	}
	if (fence_len) {
		*fence_len = run;
	}
	if (info_off) {
		*info_off = off;
	}
	if (info_len) {
		*info_len = ilen;
	}
	return 1;
}

static int md_is_hr(const char *s, size_t len)
{
	size_t i = 0;
	while (i < len && s[i] == ' ') {
		i++;
	}
	if (i >= len) {
		return 0;
	}
	char c = s[i];
	if (c != '-' && c != '*' && c != '_') {
		return 0;
	}
	size_t count = 0;
	for (; i < len; i++) {
		if (s[i] == c) {
			count++;
		} else if (s[i] != ' ') {
			return 0;
		}
	}
	return count >= 3;
}

/**
 * @brief Classifies a list item line.
 *
 * @param indent     Receives the leading-space indent.
 * @param marker_len Receives the marker length including its space.
 * @param body_off   Receives the offset of the item text.
 * @return 1 when the line starts a list item.
 */
static int md_is_list_item(const char *s, size_t len, size_t *indent,
			   size_t *marker_len, size_t *body_off)
{
	size_t ind = 0;
	while (ind < len && s[ind] == ' ') {
		ind++;
	}
	if (ind >= len || len - ind < 2) {
		return 0;
	}
	char c = s[ind];
	if ((c == '-' || c == '*' || c == '+') && s[ind + 1] == ' ') {
		*indent     = ind;
		*marker_len = 2;
		*body_off   = ind + 2;
		/* Collapse extra spaces after the marker. */
		while (*body_off < len && s[*body_off] == ' ') {
			(*body_off)++;
		}
		return 1;
	}
	if (c >= '1' && c <= '9') {
		size_t d = ind;
		while (d < len && s[d] >= '0' && s[d] <= '9') {
			d++;
		}
		if (d < len && (s[d] == '.' || s[d] == ')') && d + 1 < len &&
		    s[d + 1] == ' ') {
			*indent     = ind;
			*marker_len = d - ind + 2; /* digits + '.' + space */
			*body_off   = d + 2;
			while (*body_off < len && s[*body_off] == ' ') {
				(*body_off)++;
			}
			return 1;
		}
	}
	return 0;
}

/** Renders a blockquote line, stacked bars included. */
static void md_render_quote(md_ctx_t *ctx, const char *s, size_t len)
{
	size_t depth = 0;
	size_t i     = 0;
	while (i < len && s[i] == '>') {
		depth++;
		i++;
	}
	if (i < len && s[i] == ' ') {
		i++;
	}

	static const char bar_str[] = "\033[0;2m\xe2\x94\x82\033[0m ";
	char   bar[SGR_MAX];
	size_t bar_len = sizeof(bar_str) - 1;
	memcpy(bar, bar_str, bar_len);
	char pref[SGR_MAX * 6];
	size_t pref_len = 0;
	for (size_t d = 0; d < depth && pref_len + bar_len < sizeof(pref); d++) {
		memcpy(pref + pref_len, bar, bar_len);
		pref_len += bar_len;
	}
	pref[pref_len] = '\0';

	md_render_prefixed(ctx, s + i, len - i, 0, pref, pref);
}

static void md_render_heading(md_ctx_t *ctx, const char *s, size_t len)
{
	size_t level = 0;
	while (level < len && s[level] == '#') {
		level++;
	}
	/* ATX headings need 1-6 hashes followed by a space or end of line. */
	if (level == 0 || level > 6 || (level < len && s[level] != ' ')) {
		md_render_prefixed(ctx, s, len, 0, "", "");
		return;
	}
	size_t off = level;
	while (off < len && s[off] == ' ') {
		off++;
	}
	unsigned mask = MD_B_BOLD;
	if (level <= 2) {
		mask |= MD_B_CYAN;
	}
	md_render_prefixed(ctx, s + off, len - off, mask, "", "");
}

static void md_render_list_item(md_ctx_t *ctx, const char *s, size_t len,
				size_t indent, size_t marker_len,
				size_t body_off, int ordered)
{
	char first_prefix[96]; /* 16 spaces + marker + NUL, no truncation */
	char rest_prefix[96];
	char spaces[17];
	if (indent > 16) {
		indent = 16;
	}
	memset(spaces, ' ', indent);
	spaces[indent] = '\0';

	char marker[SGR_MAX];
	if (ordered) {
		size_t mlen = marker_len - 1; /* digits + '.' or ')' */
		if (mlen > SGR_MAX - 2) {
			mlen = SGR_MAX - 2;
		}
		memcpy(marker, s + indent, mlen);
		marker[mlen]     = ' ';
		marker[mlen + 1] = '\0';
	} else {
		memcpy(marker, "\xe2\x80\xa2 ", 4); /* "• " */
		marker[4] = '\0';
	}

	snprintf(first_prefix, sizeof(first_prefix), "%s%s", spaces, marker);
	size_t pad = md_disp_width(marker, strlen(marker));
	char   rest_tail[SGR_MAX];
	if (pad > sizeof(rest_tail) - 1) {
		pad = sizeof(rest_tail) - 1;
	}
	memset(rest_tail, ' ', pad);
	rest_tail[pad] = '\0';
	snprintf(rest_prefix, sizeof(rest_prefix), "%s%s", spaces, rest_tail);

	md_render_prefixed(ctx, s + body_off, len - body_off, 0,
			   first_prefix, rest_prefix);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

md_line_t *md_render(const char *content, int width, size_t *out_count)
{
	if (out_count) {
		*out_count = 0;
	}
	if (!content || !*content) {
		return NULL;
	}
	if (width < 1) {
		width = 1;
	}

	md_ctx_t ctx;
	memset(&ctx, 0, sizeof(ctx));
	ctx.width = width;

	const char *p = content;
	int    in_code   = 0;
	char   fence_c   = '`';
	size_t fence_len = 3;

	while (*p && !ctx.failed) {
		const char *eol = strchr(p, '\n');
		size_t llen = eol ? (size_t)(eol - p) : strlen(p);

		if (md_line_blank(p, llen)) {
			md_ctx_push_blank(&ctx);
		} else if (in_code) {
			size_t flen = 0;
			if (md_is_fence(p, llen, NULL, &flen, NULL, NULL) &&
			    flen >= fence_len) {
				/* A closing fence must carry no info string. */
				size_t io = 0, il = 0;
				md_is_fence(p, llen, NULL, NULL, &io, &il);
				if (il == 0) {
					in_code = 0;
				} else {
					md_render_code_line(&ctx, p, llen);
				}
			} else {
				md_render_code_line(&ctx, p, llen);
			}
		} else if (md_is_fence(p, llen, &fence_c, &fence_len, NULL, NULL)) {
			in_code = 1;
			size_t io = 0, il = 0;
			md_is_fence(p, llen, NULL, NULL, &io, &il);
			if (il > 0) {
				/* Show the language tag dim on the fence. */
				md_sbuf_t sb;
				md_sb_init(&sb);
				if (md_sb_text(&sb, "  ", 2, 0) == 0 &&
				    md_sb_text(&sb, p + io, il, MD_B_DIM) == 0 &&
				    md_sb_raw(&sb, "\033[0m", 4) == 0 &&
				    md_sb_reserve(&sb, 1) == 0) {
					sb.data[sb.len] = '\0';
					if (md_larr_push_text(&ctx.lines, sb.data) != 0) {
						ctx.failed = 1;
					}
				} else {
					md_sb_free(&sb);
					ctx.failed = 1;
				}
			}
		} else if (md_is_hr(p, llen)) {
			md_render_hr(&ctx);
		} else if (llen > 0 && p[0] == '>') {
			md_render_quote(&ctx, p, llen);
		} else if (llen > 0 && p[0] == '#') {
			md_render_heading(&ctx, p, llen);
		} else {
			size_t indent = 0, marker_len = 0, body_off = 0;
			if (md_is_list_item(p, llen, &indent, &marker_len,
					    &body_off)) {
				int ordered = !(p[indent] == '-' ||
						p[indent] == '*' ||
						p[indent] == '+');
				md_render_list_item(&ctx, p, llen, indent,
						    marker_len, body_off,
						    ordered);
			} else {
				md_render_prefixed(&ctx, p, llen, 0, "", "");
			}
		}

		if (!eol) {
			break;
		}
		p = eol + 1;
	}

	size_t count = ctx.lines.count;
	if (ctx.failed || count == 0) {
		md_larr_free(&ctx.lines);
		return NULL;
	}
	if (out_count) {
		*out_count = count;
	}
	md_line_t *items = ctx.lines.items;
	ctx.lines.items  = NULL; /* ownership moved to the caller */
	ctx.lines.count  = 0;
	ctx.lines.cap    = 0;
	return items;
}

void md_lines_free(md_line_t *lines, size_t count)
{
	if (!lines) {
		return;
	}
	for (size_t i = 0; i < count; i++) {
		free(lines[i].text);
	}
	free(lines);
}
