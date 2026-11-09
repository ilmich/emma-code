/**
 * @file test_theme.c
 * @brief Unit tests for src/theme.c: defaults, [theme] overrides from
 *        parsed TOML, invalid-value fallbacks and copy/free semantics.
 */

#include "test.h"

#include "theme.h"
#include "toml.h"

/* POSIX symbols not declared by strict ISO C99 headers on this toolchain
 * are declared explicitly here instead of enabling feature-test macros
 * (see AGENTS.md portability rules). */
extern char *strdup(const char *s);

#include <stdlib.h>
#include <string.h>

/* Parses a TOML document from memory; returns the table or NULL. */
static toml_table_t *parse_doc(const char *src)
{
	toml_table_t *doc = NULL;
	char errbuf[256];

	if (toml_parse_string(src, &doc, errbuf, sizeof errbuf) != 0)
		return NULL;
	return doc;
}

static void test_default_and_null_safety(void)
{
	theme_t t = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};

	/* NULL arguments are rejected without crashing. */
	TEST_CHECK_INT_EQ(theme_default(NULL), -1);
	TEST_CHECK_INT_EQ(theme_copy(NULL, &t), -1);
	TEST_CHECK_INT_EQ(theme_copy(&t, NULL), -1);
	TEST_CHECK_INT_EQ(theme_load(NULL, NULL), -1);
	theme_free(NULL);

	/* Defaults fill every field. */
	TEST_CHECK_INT_EQ(theme_default(&t), 0);
	TEST_CHECK_NOT_NULL(t.user_bg);
	TEST_CHECK_NOT_NULL(t.reasoning_on);
	TEST_CHECK_NOT_NULL(t.status);
	TEST_CHECK_NOT_NULL(t.input);
	TEST_CHECK_NOT_NULL(t.tool);
	TEST_CHECK_NOT_NULL(t.tool_output);
	TEST_CHECK_NOT_NULL(t.tool_error);
	TEST_CHECK_STR_EQ(t.tool, "\033[1;35m");
	TEST_CHECK_STR_EQ(t.tool_output, "\033[90m");
	TEST_CHECK_STR_EQ(t.tool_error, "\033[1;31m");

	/* theme_free resets every field, so a double free is safe. */
	theme_free(&t);
	TEST_CHECK_NULL(t.user_bg);
	TEST_CHECK_NULL(t.reasoning_on);
	TEST_CHECK_NULL(t.status);
	TEST_CHECK_NULL(t.input);
	TEST_CHECK_NULL(t.tool);
	TEST_CHECK_NULL(t.tool_output);
	TEST_CHECK_NULL(t.tool_error);
	theme_free(&t);
}

static void test_load_without_section_keeps_defaults(void)
{
	theme_t t = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
	toml_table_t *doc = parse_doc("base_url = \"x\"\n");
	char *saved = NULL;

	TEST_CHECK_NOT_NULL(doc);
	TEST_CHECK_INT_EQ(theme_default(&t), 0);

	/* NULL doc is a no-op success. */
	TEST_CHECK_INT_EQ(theme_load(&t, NULL), 0);

	saved = strdup(t.user_bg);
	TEST_CHECK_NOT_NULL(saved);
	TEST_CHECK_INT_EQ(theme_load(&t, doc), 0);
	TEST_CHECK_STR_EQ(t.user_bg, saved);

	free(saved);
	theme_free(&t);
	toml_free(doc);
}

static void test_load_overrides(void)
{
	theme_t t = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
	toml_table_t *doc = parse_doc(
		"[theme]\n"
		"user_bg = \"red\"\n"
		"reasoning = \"italic\"\n"
		"status = \"cyan\"\n"
		"input = \"bright_green\"\n"
		"tool = \"magenta\"\n"
		"tool_output = \"bright_black\"\n"
		"tool_error = \"red\"\n");

	TEST_CHECK_NOT_NULL(doc);
	TEST_CHECK_INT_EQ(theme_default(&t), 0);
	TEST_CHECK_INT_EQ(theme_load(&t, doc), 0);

	TEST_CHECK_STR_EQ(t.user_bg, "\033[41m");
	TEST_CHECK_STR_EQ(t.reasoning_on, "\033[3m");
	TEST_CHECK_STR_EQ(t.status, "\033[36m");
	TEST_CHECK_STR_EQ(t.input, "\033[92m");
	TEST_CHECK_STR_EQ(t.tool, "\033[35m");
	TEST_CHECK_STR_EQ(t.tool_output, "\033[90m");
	TEST_CHECK_STR_EQ(t.tool_error, "\033[31m");

	theme_free(&t);
	toml_free(doc);
}

static void test_load_numeric_colors(void)
{
	theme_t t = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
	toml_table_t *doc = parse_doc(
		"[theme]\n"
		"user_bg = \"on:17\"\n"
		"status = \"196\"\n");

	TEST_CHECK_NOT_NULL(doc);
	TEST_CHECK_INT_EQ(theme_default(&t), 0);
	TEST_CHECK_INT_EQ(theme_load(&t, doc), 0);

	TEST_CHECK_STR_EQ(t.user_bg, "\033[48;5;17m");
	TEST_CHECK_STR_EQ(t.status, "\033[38;5;196m");

	theme_free(&t);
	toml_free(doc);
}

static void test_load_style_names(void)
{
	theme_t t = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
	toml_table_t *doc = parse_doc(
		"[theme]\n"
		"reasoning = \"dim\"\n");

	TEST_CHECK_NOT_NULL(doc);
	TEST_CHECK_INT_EQ(theme_default(&t), 0);
	TEST_CHECK_INT_EQ(theme_load(&t, doc), 0);
	TEST_CHECK_STR_EQ(t.reasoning_on, "\033[2m");

	theme_free(&t);
	toml_free(doc);
}

static void test_load_invalid_keeps_defaults(void)
{
	theme_t t = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
	toml_table_t *doc = parse_doc(
		"[theme]\n"
		"user_bg = \"notacolor\"\n"
		"reasoning = \"blink\"\n"
		"status = \"abc\"\n"
		"input = \"on:x\"\n"
		"tool = \"notacolor\"\n"
		"tool_output = \"on:zz\"\n"
		"tool_error = \"999\"\n");

	TEST_CHECK_NOT_NULL(doc);
	TEST_CHECK_INT_EQ(theme_default(&t), 0);
	TEST_CHECK_INT_EQ(theme_load(&t, doc), 0);

	/* Invalid values keep the defaults; startup never fails. */
	TEST_CHECK_STR_EQ(t.user_bg, "\033[44m");
	TEST_CHECK_STR_EQ(t.reasoning_on, "\033[2;3m");
	TEST_CHECK_STR_EQ(t.status, "\033[1;33m");
	TEST_CHECK_STR_EQ(t.input, "\033[1;32m");
	TEST_CHECK_STR_EQ(t.tool, "\033[1;35m");
	TEST_CHECK_STR_EQ(t.tool_output, "\033[90m");
	TEST_CHECK_STR_EQ(t.tool_error, "\033[1;31m");

	theme_free(&t);
	toml_free(doc);
}

static void test_load_partial_section(void)
{
	theme_t t = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
	toml_table_t *doc = parse_doc(
		"[theme]\n"
		"user_bg = \"magenta\"\n");

	TEST_CHECK_NOT_NULL(doc);
	TEST_CHECK_INT_EQ(theme_default(&t), 0);
	TEST_CHECK_INT_EQ(theme_load(&t, doc), 0);

	/* Only the given key changes; the others stay at defaults. */
	TEST_CHECK_STR_EQ(t.user_bg, "\033[45m");
	TEST_CHECK_STR_EQ(t.reasoning_on, "\033[2;3m");
	TEST_CHECK_STR_EQ(t.status, "\033[1;33m");
	TEST_CHECK_STR_EQ(t.input, "\033[1;32m");
	TEST_CHECK_STR_EQ(t.tool, "\033[1;35m");
	TEST_CHECK_STR_EQ(t.tool_output, "\033[90m");
	TEST_CHECK_STR_EQ(t.tool_error, "\033[1;31m");

	theme_free(&t);
	toml_free(doc);
}

static void test_copy_and_free(void)
{
	theme_t a = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
	theme_t b = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};

	TEST_CHECK_INT_EQ(theme_default(&a), 0);
	TEST_CHECK_INT_EQ(theme_copy(&b, &a), 0);

	TEST_CHECK_STR_EQ(b.user_bg, a.user_bg);
	TEST_CHECK_STR_EQ(b.reasoning_on, a.reasoning_on);
	TEST_CHECK_STR_EQ(b.status, a.status);
	TEST_CHECK_STR_EQ(b.input, a.input);
	TEST_CHECK_STR_EQ(b.tool, a.tool);
	TEST_CHECK_STR_EQ(b.tool_output, a.tool_output);
	TEST_CHECK_STR_EQ(b.tool_error, a.tool_error);

	/* The copy is deep: freeing the source leaves it usable. */
	theme_free(&a);
	TEST_CHECK_NOT_NULL(b.user_bg);
	TEST_CHECK_STR_EQ(b.user_bg, "\033[44m");

	theme_free(&b);
}

int main(void)
{
	TEST_RUN(test_default_and_null_safety);
	TEST_RUN(test_load_without_section_keeps_defaults);
	TEST_RUN(test_load_overrides);
	TEST_RUN(test_load_numeric_colors);
	TEST_RUN(test_load_style_names);
	TEST_RUN(test_load_invalid_keeps_defaults);
	TEST_RUN(test_load_partial_section);
	TEST_RUN(test_copy_and_free);

	if (test__failures == 0)
		printf("All theme tests passed\n");
	return test__failures ? 1 : 0;
}
