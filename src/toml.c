/**
 * @file toml.c
 * @brief Hand-rolled parser for the practical TOML subset (include/toml.h).
 *
 * Line-oriented recursive-descent parser, plain C99, no dynamic globals.
 * All parse output is owned by the returned toml_table_t; the caller frees
 * it with toml_free(). Getter results borrow from the table and are never
 * freed by the caller.
 */

#include "toml.h"
#include "util.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Maximum dotted-key depth accepted by the parser. */
#define TOML_MAX_DEPTH 32

/** Maximum length of a bare key component (including the NUL). */
#define TOML_MAX_KEY 128

/** Maximum array nesting depth. */
#define TOML_MAX_ARRAY_DEPTH 8

typedef struct toml_value {
	toml_type_t type;
	char *s;               /* TOML_STRING */
	long long i;           /* TOML_INT */
	double d;              /* TOML_DOUBLE */
	int b;                 /* TOML_BOOL */
	toml_table_t *tbl;     /* TOML_TABLE */
	toml_array_t view;     /* TOML_ARRAY: stable accessor */
	struct toml_value *items; /* TOML_ARRAY items (scalars only) */
	size_t count;          /* TOML_ARRAY item count */
} toml_value_t;

typedef struct toml_entry {
	char *key;
	toml_value_t val;
} toml_entry_t;

struct toml_table {
	toml_entry_t *entries;
	size_t count;
	size_t cap;
	int defined; /* defined through a [table] header */
};

struct parser {
	const char *begin;
	const char *p;
	const char *end;
	char *err;
	size_t errlen;
	int failed;
};

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */

/**
 * Formats an error message into the caller buffer. vsnprintf always
 * NUL-terminates, but on truncation the message is incomplete: it is
 * still used (parse errors are fatal anyway) and additionally reported
 * on stderr.
 */
static void set_err(char *errbuf, size_t errlen, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (errbuf == NULL || errlen == 0)
		return;
	va_start(ap, fmt);
	n = vsnprintf(errbuf, errlen, fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= errlen)
		fprintf(stderr, "toml: error message truncated\n");
}

/** Reports an error at the current parser position (line number). */
static void p_err(struct parser *ps, const char *fmt, ...)
{
	va_list ap;
	long line = 1;
	const char *q;
	int n;

	for (q = ps->begin; q < ps->p && q < ps->end; q++)
		if (*q == '\n')
			line++;

	if (ps->err != NULL && ps->errlen > 0) {
		n = snprintf(ps->err, ps->errlen, "line %ld: ", line);
		if (n >= 0 && (size_t)n < ps->errlen) {
			size_t off = (size_t)n;

			va_start(ap, fmt);
			n = vsnprintf(ps->err + off, ps->errlen - off, fmt, ap);
			va_end(ap);
			if (n >= 0 && (size_t)n < ps->errlen - off) {
				ps->failed = 1;
				return;
			}
		}
		/* Prefix or body truncated: vsnprintf still NUL-terminated
		 * the buffer, but surface the loss on stderr as well. */
		fprintf(stderr, "toml: error message truncated\n");
	}
	ps->failed = 1;
}

/* ------------------------------------------------------------------ */
/* Character classes                                                   */

static int p_is_bare_start(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       c == '_' || (c >= '0' && c <= '9');
}

static int p_is_bare_char(char c)
{
	return p_is_bare_start(c) || c == '-';
}

/** Characters that may appear inside a number token. */
static int p_is_numchar(char c)
{
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
	       (c >= 'A' && c <= 'Z') || c == '_' || c == '+' || c == '-' ||
	       c == '.';
}

static int p_hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Lexical helpers                                                     */

/** Skips spaces, tabs and CR (never a newline). */
static void p_inline_ws(struct parser *ps)
{
	while (ps->p < ps->end &&
	       (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\r'))
		ps->p++;
}

/** Skips to end of line if a comment is pending. */
static void p_comment(struct parser *ps)
{
	if (ps->p < ps->end && *ps->p == '#')
		while (ps->p < ps->end && *ps->p != '\n')
			ps->p++;
}

/**
 * Skips inline whitespace and an optional trailing comment, then requires
 * the line to end (newline or EOF). Returns 0 on success, -1 on error.
 */
static int p_skip_trailing(struct parser *ps)
{
	p_inline_ws(ps);
	p_comment(ps);
	if (ps->p < ps->end && *ps->p != '\n') {
		p_err(ps, "trailing characters on line");
		return -1;
	}
	return 0;
}

/** Skips blank lines and comment lines; stops at content or EOF. */
static void p_skip_content(struct parser *ps)
{
	for (;;) {
		p_inline_ws(ps);
		p_comment(ps);
		if (ps->p < ps->end && *ps->p == '\n') {
			ps->p++;
			continue;
		}
		break;
	}
}

static int p_value(struct parser *ps, toml_value_t *out, int depth);

/* ------------------------------------------------------------------ */
/* Table primitives                                                    */

static toml_table_t *t_new(void)
{
	return calloc(1, sizeof(toml_table_t));
}

static void t_free_value(toml_value_t *v)
{
	size_t j;

	free(v->s);
	if (v->type == TOML_ARRAY && v->items != NULL) {
		for (j = 0; j < v->count; j++)
			t_free_value(&v->items[j]);
		free(v->items);
	}
	toml_free(v->tbl);
}

void toml_free(toml_table_t *t)
{
	size_t i;

	if (t == NULL)
		return;
	for (i = 0; i < t->count; i++) {
		free(t->entries[i].key);
		t_free_value(&t->entries[i].val);
	}
	free(t->entries);
	free(t);
}

static toml_entry_t *t_find(const toml_table_t *t, const char *key)
{
	size_t i;

	for (i = 0; i < t->count; i++)
		if (strcmp(t->entries[i].key, key) == 0)
			return (toml_entry_t *)&t->entries[i];
	return NULL;
}

static int t_grow(toml_table_t *t)
{
	size_t cap = t->cap == 0 ? 8 : t->cap * 2;
	toml_entry_t *e = realloc(t->entries, cap * sizeof *e);

	if (e == NULL)
		return -1;
	t->entries = e;
	t->cap = cap;
	return 0;
}

/**
 * Inserts a value under `name`, taking ownership of it. Fails when the
 * key already exists or on allocation failure.
 */
static int t_insert(toml_table_t *t, const char *name, toml_value_t v)
{
	toml_entry_t *e;
	char *k;

	if (t_find(t, name) != NULL)
		return -1;
	if (t->count == t->cap && t_grow(t) != 0)
		return -1;
	k = util_strdup(name);
	if (k == NULL)
		return -1;
	e = &t->entries[t->count++];
	e->key = k;
	e->val = v;
	return 0;
}

/**
 * Returns the sub-table `name`, creating an (implicit) empty one when
 * missing. Returns NULL with an error reported when `name` exists as a
 * non-table entry.
 */
static toml_table_t *t_descend(toml_table_t *t, const char *name,
			       struct parser *ps)
{
	toml_entry_t *e = t_find(t, name);
	toml_table_t *sub;

	if (e != NULL) {
		if (e->val.type != TOML_TABLE) {
			p_err(ps, "'%s' is not a table", name);
			return NULL;
		}
		return e->val.tbl;
	}
	sub = t_new();
	if (sub == NULL) {
		p_err(ps, "out of memory");
		return NULL;
	}
	{
		toml_value_t v = {0};

		v.type = TOML_TABLE;
		v.tbl = sub;
		if (t_insert(t, name, v) != 0) {
			toml_free(sub);
			p_err(ps, "out of memory");
			return NULL;
		}
	}
	return sub;
}

/** Const payload pointer for a value (borrows from the value). */
static const void *t_payload(const toml_value_t *v)
{
	switch (v->type) {
	case TOML_STRING:
		return v->s;
	case TOML_INT:
		return &v->i;
	case TOML_DOUBLE:
		return &v->d;
	case TOML_BOOL:
		return &v->b;
	case TOML_ARRAY:
		return &v->view;
	case TOML_TABLE:
		return v->tbl;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Bounded string building (strings never exceed their source span)    */

static int sb_putc(char *buf, size_t cap, size_t *len, char c)
{
	if (*len + 1 >= cap)
		return -1;
	buf[(*len)++] = c;
	return 0;
}

/** Encodes a code point as UTF-8. Returns -1 on invalid code points. */
static int utf8_put(unsigned cp, char *buf, size_t cap, size_t *len)
{
	if (cp < 0x80)
		return sb_putc(buf, cap, len, (char)cp);
	if (cp < 0x800) {
		if (sb_putc(buf, cap, len, (char)(0xC0 | (cp >> 6))) != 0)
			return -1;
		return sb_putc(buf, cap, len, (char)(0x80 | (cp & 0x3F)));
	}
	if (cp < 0x10000) {
		if (sb_putc(buf, cap, len, (char)(0xE0 | (cp >> 12))) != 0 ||
		    sb_putc(buf, cap, len,
			    (char)(0x80 | ((cp >> 6) & 0x3F))) != 0)
			return -1;
		return sb_putc(buf, cap, len, (char)(0x80 | (cp & 0x3F)));
	}
	if (cp <= 0x10FFFF) {
		if (sb_putc(buf, cap, len, (char)(0xF0 | (cp >> 18))) != 0 ||
		    sb_putc(buf, cap, len,
			    (char)(0x80 | ((cp >> 12) & 0x3F))) != 0 ||
		    sb_putc(buf, cap, len,
			    (char)(0x80 | ((cp >> 6) & 0x3F))) != 0)
			return -1;
		return sb_putc(buf, cap, len, (char)(0x80 | (cp & 0x3F)));
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Value parsing                                                       */

static int p_basic_string(struct parser *ps, toml_value_t *out)
{
	/* Output never exceeds the source span: escapes consume at least
	 * 2 input bytes per output byte, \u 6 for at most 4. Guard anyway
	 * and abort the string on any bound violation. */
	size_t cap = (size_t)(ps->end - ps->p);
	size_t len = 0;
	char *buf = NULL;
	int rc = -1;

	ps->p++; /* opening quote */
	buf = malloc(cap);
	if (buf == NULL) {
		p_err(ps, "out of memory");
		return -1; /* nothing to clean up yet */
	}
	while (ps->p < ps->end && *ps->p != '"') {
		char c = *ps->p;

		if (c == '\n') {
			p_err(ps, "unterminated string");
			goto cleanup;
		}
		ps->p++;
		if (c != '\\') {
			if (sb_putc(buf, cap, &len, c) != 0) {
				p_err(ps, "string too long");
				goto cleanup;
			}
			continue;
		}
		if (ps->p >= ps->end) {
			p_err(ps, "unterminated string");
			goto cleanup;
		}
		c = *ps->p++; 
		switch (c) {
		case 'b':
			c = '\b';
			break;
		case 't':
			c = '\t';
			break;
		case 'n':
			c = '\n';
			break;
		case 'f':
			c = '\f';
			break;
		case 'r':
			c = '\r';
			break;
		case '"':
			break;
		case '\\':
			break;
		case 'u':
		case 'U': {
			int hex = c == 'u' ? 4 : 8;
			unsigned cp = 0;
			int k;

			if (ps->end - ps->p < hex) {
				p_err(ps, "truncated \\u escape");
				goto cleanup;
			}
			for (k = 0; k < hex; k++) {
				int d = p_hexval(*ps->p++);

				if (d < 0) {
					p_err(ps, "invalid \\u escape");
					goto cleanup;
				}
				cp = cp * 16 + (unsigned)d;
			}
			if (cp >= 0xD800 && cp <= 0xDFFF) {
				p_err(ps, "surrogate in \\u escape");
				goto cleanup;
			}
			if (cp == 0) {
				/* Strings are NUL-terminated C strings: an embedded
				 * NUL would silently truncate every getter. */
				p_err(ps, "NUL in \\u escape");
				goto cleanup;
			}
			if (utf8_put(cp, buf, cap, &len) != 0) {
				p_err(ps, "invalid code point in \\u escape");
				goto cleanup;
			}
			continue;
		}
		default:
			p_err(ps, "invalid escape '\\%c'", c);
			goto cleanup;
		}
		if (sb_putc(buf, cap, &len, c) != 0) {
			p_err(ps, "string too long");
			goto cleanup;
		}
	}
	if (ps->p >= ps->end) {
		p_err(ps, "unterminated string");
		goto cleanup;
	}
	ps->p++; /* closing quote */
	buf[len] = '\0';
	out->type = TOML_STRING;
	out->s = buf;
	buf = NULL; /* ownership transferred to out */
	rc = 0;

cleanup:
	free(buf);
	return rc;
}

static int p_literal_string(struct parser *ps, toml_value_t *out)
{
	const char *start;

	ps->p++; /* opening quote */
	start = ps->p;
	while (ps->p < ps->end && *ps->p != '\'' && *ps->p != '\n')
		ps->p++;
	if (ps->p >= ps->end || *ps->p != '\'') {
		p_err(ps, "unterminated string");
		return -1;
	}
	{
		size_t n = (size_t)(ps->p - start);

		out->type = TOML_STRING;
		out->s = malloc(n + 1);
		if (out->s == NULL) {
			p_err(ps, "out of memory");
			return -1;
		}
		memcpy(out->s, start, n);
		out->s[n] = '\0';
	}
	ps->p++;
	return 0;
}

static int p_bool(struct parser *ps, toml_value_t *out)
{
	size_t rem = (size_t)(ps->end - ps->p);

	if (rem >= 4 && strncmp(ps->p, "true", 4) == 0) {
		ps->p += 4;
		out->type = TOML_BOOL;
		out->b = 1;
		return 0;
	}
	if (rem >= 5 && strncmp(ps->p, "false", 5) == 0) {
		ps->p += 5;
		out->type = TOML_BOOL;
		out->b = 0;
		return 0;
	}
	p_err(ps, "invalid value");
	return -1;
}

static int p_number(struct parser *ps, toml_value_t *out)
{
	char tok[TOML_MAX_KEY];
	char num[TOML_MAX_KEY];
	size_t n = 0, m = 0, i;
	char *endp;

	while (ps->p < ps->end && p_is_numchar(*ps->p)) {
		if (n + 1 >= sizeof tok) {
			p_err(ps, "number too long");
			return -1;
		}
		tok[n++] = *ps->p++;
	}
	tok[n] = '\0';
	if (n == 0) {
		p_err(ps, "expected value");
		return -1;
	}
	if (strchr(tok, ':') != NULL) {
		p_err(ps, "dates and times are not supported");
		return -1;
	}
	if (n > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
		p_err(ps, "hexadecimal numbers are not supported");
		return -1;
	}
	/* Strip TOML digit separators before conversion. */
	for (i = 0; i < n; i++) {
		if (tok[i] == '_')
			continue;
		if (m + 1 >= sizeof num) {
			p_err(ps, "number too long");
			return -1;
		}
		num[m++] = tok[i];
	}
	num[m] = '\0';

	if (strpbrk(num, ".eE") != NULL) {
		double d;

		errno = 0;
		d = strtod(num, &endp);
		if (endp == num || *endp != '\0') {
			p_err(ps, "invalid number '%s'", tok);
			return -1;
		}
		if (errno == ERANGE && (d == HUGE_VAL || d == -HUGE_VAL)) {
			p_err(ps, "number '%s' out of range", tok);
			return -1;
		}
		out->type = TOML_DOUBLE;
		out->d = d;
		return 0;
	}
	{
		long long v;

		errno = 0;
		v = strtoll(num, &endp, 10);
		if (endp == num || *endp != '\0') {
			p_err(ps, "invalid integer '%s'", tok);
			return -1;
		}
		if (errno == ERANGE) {
			p_err(ps, "integer '%s' out of range", tok);
			return -1;
		}
		out->type = TOML_INT;
		out->i = v;
	}
	return 0;
}

/** Frees the item values of a partially built array, then the array. */
static void array_discard(toml_value_t *items, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		t_free_value(&items[i]);
	free(items);
}

static int p_array(struct parser *ps, toml_value_t *out, int depth)
{
	toml_value_t *items = NULL;
	size_t count = 0, cap = 0;

	ps->p++; /* '[' */
	if (depth >= TOML_MAX_ARRAY_DEPTH) {
		p_err(ps, "array nesting too deep");
		return -1;
	}
	for (;;) {
		toml_value_t v = {0};

		/* Arrays may span lines. */
		for (;;) {
			p_inline_ws(ps);
			p_comment(ps);
			if (ps->p < ps->end && *ps->p == '\n') {
				ps->p++;
				continue;
			}
			break;
		}
		if (ps->p >= ps->end) {
			array_discard(items, count);
			p_err(ps, "unterminated array");
			return -1;
		}
		if (*ps->p == ']') {
			ps->p++;
			break;
		}
		if (p_value(ps, &v, depth + 1) != 0) {
			array_discard(items, count);
			return -1;
		}
		if (count == cap) {
			size_t ncap = cap == 0 ? 8 : cap * 2;
			toml_value_t *ni = realloc(items,
						   ncap * sizeof *ni);

			if (ni == NULL) {
				t_free_value(&v);
				array_discard(items, count);
				p_err(ps, "out of memory");
				return -1;
			}
			items = ni;
			cap = ncap;
		}
		items[count++] = v;
		for (;;) {
			p_inline_ws(ps);
			p_comment(ps);
			if (ps->p < ps->end && *ps->p == '\n') {
				ps->p++;
				continue;
			}
			break;
		}
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == ']') {
			ps->p++;
			break;
		}
		array_discard(items, count);
		p_err(ps, "expected ',' or ']' in array");
		return -1;
	}
	out->type = TOML_ARRAY;
	out->items = items;
	out->count = count;
	out->view.items = items;
	out->view.count = count;
	return 0;
}

static int p_value(struct parser *ps, toml_value_t *out, int depth)
{
	char c;

	p_inline_ws(ps);
	if (ps->p >= ps->end) {
		p_err(ps, "expected value");
		return -1;
	}
	c = *ps->p;
	if (c == '"')
		return p_basic_string(ps, out);
	if (c == '\'')
		return p_literal_string(ps, out);
	if (c == '[') {
		if (depth > 0) {
			p_err(ps, "nested arrays are not supported");
			return -1;
		}
		return p_array(ps, out, depth);
	}
	if (c == '{') {
		p_err(ps, "inline tables are not supported");
		return -1;
	}
	if (c == 't' || c == 'f')
		return p_bool(ps, out);
	if ((c >= '0' && c <= '9') || c == '+' || c == '-')
		return p_number(ps, out);
	p_err(ps, "unexpected character '%c'", c);
	return -1;
}

/* ------------------------------------------------------------------ */
/* Keys and top-level structure                                        */

/**
 * Parses a dotted key path into `names`. The parser stops right after
 * the last component. Returns the component count, or -1 on error.
 */
static int p_key_path(struct parser *ps, char names[][TOML_MAX_KEY], int max)
{
	int n = 0;

	for (;;) {
		p_inline_ws(ps);
		if (p_is_bare_start(*ps->p) == 0) {
			p_err(ps, "expected key");
			return -1;
		}
		if (n == max) {
			p_err(ps, "dotted key too deep");
			return -1;
		}
		{
			size_t len = 0;

			while (ps->p < ps->end && p_is_bare_char(*ps->p)) {
				if (len + 1 >= TOML_MAX_KEY) {
					p_err(ps, "key too long");
					return -1;
				}
				names[n][len++] = *ps->p++;
			}
			names[n][len] = '\0';
		}
		n++;
		p_inline_ws(ps);
		if (ps->p < ps->end && *ps->p == '.') {
			ps->p++;
			continue;
		}
		return n;
	}
}

static int p_table_header(struct parser *ps, toml_table_t *root,
			  toml_table_t **cur)
{
	char names[TOML_MAX_DEPTH][TOML_MAX_KEY];
	int n, i;
	toml_table_t *t = root;

	ps->p++; /* '[' */
	n = p_key_path(ps, names, TOML_MAX_DEPTH);
	if (n < 0)
		return -1;
	p_inline_ws(ps);
	if (ps->p >= ps->end || *ps->p != ']') {
		p_err(ps, "expected ']'");
		return -1;
	}
	ps->p++;
	if (p_skip_trailing(ps) != 0)
		return -1;

	for (i = 0; i < n - 1; i++) {
		t = t_descend(t, names[i], ps);
		if (t == NULL)
			return -1;
	}
	{
		toml_entry_t *e = t_find(t, names[n - 1]);

		if (e != NULL) {
			if (e->val.type != TOML_TABLE) {
				p_err(ps, "'%s' is not a table",
				      names[n - 1]);
				return -1;
			}
			if (e->val.tbl->defined) {
				p_err(ps, "table '%s' defined twice",
				      names[n - 1]);
				return -1;
			}
			t = e->val.tbl;
		} else {
			t = t_descend(t, names[n - 1], ps);
			if (t == NULL)
				return -1;
		}
		t->defined = 1;
	}
	*cur = t;
	return 0;
}

static int p_keyval(struct parser *ps, toml_table_t **cur)
{
	char names[TOML_MAX_DEPTH][TOML_MAX_KEY];
	toml_value_t v = {0};
	toml_table_t *t = *cur;
	int n, i;

	n = p_key_path(ps, names, TOML_MAX_DEPTH);
	if (n < 0)
		return -1;
	p_inline_ws(ps);
	if (ps->p >= ps->end || *ps->p != '=') {
		p_err(ps, "expected '='");
		return -1;
	}
	ps->p++;
	if (p_value(ps, &v, 0) != 0)
		return -1;
	if (p_skip_trailing(ps) != 0) {
		t_free_value(&v);
		return -1;
	}

	for (i = 0; i < n - 1; i++) {
		toml_entry_t *e = t_find(t, names[i]);

		if (e != NULL) {
			if (e->val.type != TOML_TABLE) {
				p_err(ps, "'%s' is not a table", names[i]);
				t_free_value(&v);
				return -1;
			}
			if (e->val.tbl->defined) {
				p_err(ps, "cannot extend table '%s' defined "
					  "by a header", names[i]);
				t_free_value(&v);
				return -1;
			}
			t = e->val.tbl;
		} else {
			t = t_descend(t, names[i], ps);
			if (t == NULL) {
				t_free_value(&v);
				return -1;
			}
		}
	}
	if (t_find(t, names[n - 1]) != NULL) {
		t_free_value(&v);
		p_err(ps, "key '%s' defined twice", names[n - 1]);
		return -1;
	}
	if (t_insert(t, names[n - 1], v) != 0) {
		t_free_value(&v);
		p_err(ps, "out of memory");
		return -1;
	}
	return 0;
}

int toml_parse_string(const char *src, toml_table_t **out, char *errbuf,
		      size_t errlen)
{
	struct parser ps;
	toml_table_t *root;
	toml_table_t *cur;
	int rc = -1;

	if (src == NULL || out == NULL) {
		set_err(errbuf, errlen, "invalid argument");
		return -1;
	}
	*out = NULL;
	root = t_new();
	if (root == NULL) {
		set_err(errbuf, errlen, "out of memory");
		return -1;
	}
	ps.begin = src;
	ps.p = src;
	ps.end = src + strlen(src);
	ps.err = errbuf;
	ps.errlen = errlen;
	ps.failed = 0;
	cur = root;

	for (;;) {
		p_skip_content(&ps);
		if (ps.p >= ps.end)
			break;
		if (*ps.p == '[') {
			toml_table_t *t;

			if (p_table_header(&ps, root, &t) != 0)
				goto cleanup;
			cur = t;
		} else {
			if (p_keyval(&ps, &cur) != 0)
				goto cleanup;
		}
	}
	*out = root;
	root = NULL; /* ownership transferred to out */
	rc = 0;

cleanup:
	toml_free(root);
	return rc;
}

int toml_parse_file(const char *path, toml_table_t **out, char *errbuf,
		    size_t errlen)
{
	FILE *f = NULL;
	long sz;
	char *buf = NULL;
	int rc = -1;

	if (path == NULL || out == NULL) {
		set_err(errbuf, errlen, "invalid argument");
		return -1;
	}
	*out = NULL;
	f = fopen(path, "rb");
	if (f == NULL) {
		set_err(errbuf, errlen, "cannot open '%s'", path);
		return -1; /* nothing to clean up yet */
	}
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		set_err(errbuf, errlen, "cannot read '%s'", path);
		goto cleanup;
	}
	buf = malloc((size_t)sz + 1);
	if (buf == NULL) {
		set_err(errbuf, errlen, "out of memory");
		goto cleanup;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		set_err(errbuf, errlen, "cannot read '%s'", path);
		goto cleanup;
	}
	buf[sz] = '\0';
	rc = toml_parse_string(buf, out, errbuf, errlen);

cleanup:
	fclose(f);
	free(buf);
	return rc;
}

/* ------------------------------------------------------------------ */
/* Public getters                                                      */

const void *toml_get(const toml_table_t *t, const char *key,
		     toml_type_t *type)
{
	char comp[TOML_MAX_KEY];
	const toml_table_t *tab = t;
	const char *k = key;
	toml_entry_t *e;

	if (t == NULL || key == NULL)
		return NULL;
	for (;;) {
		const char *dot = strchr(k, '.');
		size_t n = dot != NULL ? (size_t)(dot - k) : strlen(k);

		if (n == 0 || n >= sizeof comp)
			return NULL;
		memcpy(comp, k, n);
		comp[n] = '\0';
		e = t_find((toml_table_t *)tab, comp);
		if (e == NULL)
			return NULL;
		if (dot == NULL) {
			if (type != NULL)
				*type = e->val.type;
			return t_payload(&e->val);
		}
		if (e->val.type != TOML_TABLE)
			return NULL;
		tab = e->val.tbl;
		k = dot + 1;
		if (*k == '\0')
			return NULL;
	}
}

int toml_get_string(const toml_table_t *t, const char *key, const char **out)
{
	toml_type_t type;
	const void *p;

	if (out == NULL)
		return -1;
	p = toml_get(t, key, &type);

	if (p == NULL || type != TOML_STRING)
		return -1;
	*out = p;
	return 0;
}

int toml_get_int(const toml_table_t *t, const char *key, long long *out)
{
	toml_type_t type;
	const void *p;

	if (out == NULL)
		return -1;
	p = toml_get(t, key, &type);

	if (p == NULL || type != TOML_INT)
		return -1;
	*out = *(const long long *)p;
	return 0;
}

int toml_get_double(const toml_table_t *t, const char *key, double *out)
{
	toml_type_t type;
	const void *p;

	if (out == NULL)
		return -1;
	p = toml_get(t, key, &type);

	if (p == NULL || type != TOML_DOUBLE)
		return -1;
	*out = *(const double *)p;
	return 0;
}

int toml_get_bool(const toml_table_t *t, const char *key, int *out)
{
	toml_type_t type;
	const void *p;

	if (out == NULL)
		return -1;
	p = toml_get(t, key, &type);

	if (p == NULL || type != TOML_BOOL)
		return -1;
	*out = *(const int *)p;
	return 0;
}

size_t toml_array_count(const toml_array_t *arr)
{
	return arr != NULL ? arr->count : 0;
}

const void *toml_array_item(const toml_array_t *arr, size_t idx,
			    toml_type_t *type)
{
	const toml_value_t *v;

	if (arr == NULL || idx >= arr->count || arr->items == NULL)
		return NULL;
	v = &((const toml_value_t *)arr->items)[idx];
	if (v->type != TOML_STRING && v->type != TOML_INT &&
	    v->type != TOML_DOUBLE && v->type != TOML_BOOL)
		return NULL;
	if (type != NULL)
		*type = v->type;
	return t_payload(v);
}
