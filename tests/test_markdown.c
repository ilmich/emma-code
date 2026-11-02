/**
 * @file test_markdown.c
 * @brief Unit tests for the markdown renderer (src/markdown.c).
 */

#include "test.h"

#include "markdown.h"

#include <stdlib.h>
#include <string.h>

/** Display width of a styled line: skips SGR sequences and UTF-8
 *  continuation bytes. */
static int disp_width(const char *s)
{
	int w = 0;
	for (const char *p = s; p && *p;) {
		if (*p == '\033') {
			while (*p && *p != 'm') {
				p++;
			}
			if (*p) {
				p++;
			}
			continue;
		}
		if ((((unsigned char)*p) & 0xC0) != 0x80) {
			w++;
		}
		p++;
	}
	return w;
}

/** Prints the produced lines for diagnosing failures. */
static void dump_lines(const char *label, md_line_t *lines, size_t count)
{
	if (test__failures == 0) {
		return;
	}
	fprintf(stderr, "  [%s] %zu line(s):\n", label, count);
	for (size_t i = 0; i < count; i++) {
		fprintf(stderr, "    %zu: '%s'\n", i,
			lines && lines[i].text ? lines[i].text : "(null)");
	}
}

static void test_null_and_empty(void)
{
	size_t n = 99;

	TEST_CHECK_NULL(md_render(NULL, 80, &n));
	TEST_CHECK_INT_EQ(n, 0);

	n = 99;
	TEST_CHECK_NULL(md_render("", 80, &n));
	TEST_CHECK_INT_EQ(n, 0);

	/* Degenerate width is clamped to 1: one column per line. */
	n = 0;
	md_line_t *lines = md_render("hi", 0, &n);
	TEST_CHECK_NOT_NULL(lines);
	TEST_CHECK_INT_EQ(n, 2);
	TEST_CHECK_STR_EQ(lines[0].text, "h");
	TEST_CHECK_STR_EQ(lines[1].text, "i");
	md_lines_free(lines, n);

	n = 0;
	lines = md_render("hi", -5, &n);
	TEST_CHECK_NOT_NULL(lines);
	TEST_CHECK_INT_EQ(n, 2);
	md_lines_free(lines, n);

	/* NULL-safe free. */
	md_lines_free(NULL, 0);
}

static void test_plain_text(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("hello world", 80, &n);
	TEST_CHECK_NOT_NULL(lines);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "hello world");
	TEST_CHECK_INT_EQ(disp_width(lines[0].text), 11);
	md_lines_free(lines, n);
}

static void test_heading(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("# Title", 80, &n);
	TEST_CHECK_NOT_NULL(lines);
	TEST_CHECK_INT_EQ(n, 1);
	/* Level 1: bold + cyan, marker stripped, reset at end. */
	TEST_CHECK_STR_EQ(lines[0].text, "\033[0;1;36mTitle\033[0m");
	md_lines_free(lines, n);

	n = 0;
	lines = md_render("### sub", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "\033[0;1msub\033[0m");
	md_lines_free(lines, n);

	n = 0;
	lines = md_render("######h6nospace", 80, &n);
	/* No space after the hashes: not a heading, literal text. */
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "######h6nospace");
	md_lines_free(lines, n);
}

static void test_bold_italic(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("**b**", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "\033[0;1mb\033[0m");
	md_lines_free(lines, n);

	n = 0;
	lines = md_render("*i*", 80, &n);
	TEST_CHECK_STR_EQ(lines[0].text, "\033[0;3mi\033[0m");
	md_lines_free(lines, n);

	n = 0;
	lines = md_render("_u_ ok", 80, &n);
	TEST_CHECK_STR_EQ(lines[0].text, "\033[0;3mu\033[0m ok");
	md_lines_free(lines, n);

	/* Intraword underscores stay literal. */
	n = 0;
	lines = md_render("snake_case_name", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "snake_case_name");
	md_lines_free(lines, n);
}

static void test_inline_code(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("use `x` here", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "use \033[0;35mx\033[0m here");
	md_lines_free(lines, n);
}

static void test_nested_styles(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("**bold *it***", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	/* Bold + italic must compose mid-line. */
	TEST_CHECK(strstr(lines[0].text, "\033[0;1;3m") != NULL);
	TEST_CHECK_STR_EQ(lines[0].text + strlen(lines[0].text) - 4,
			  "\033[0m");
	md_lines_free(lines, n);
}

static void test_unclosed_constructs(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("**abc", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "**abc");
	md_lines_free(lines, n);

	n = 0;
	lines = md_render("`abc", 80, &n);
	TEST_CHECK_STR_EQ(lines[0].text, "`abc");
	md_lines_free(lines, n);

	n = 0;
	lines = md_render("*abc", 80, &n);
	TEST_CHECK_STR_EQ(lines[0].text, "*abc");
	md_lines_free(lines, n);
}

static void test_code_fence(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("```c\nint x;\n```", 80, &n);
	dump_lines("code_fence", lines, n);
	TEST_CHECK_INT_EQ(n, 2);
	/* Language tag line, dim. */
	TEST_CHECK_STR_EQ(lines[0].text, "  \033[0;2mc\033[0m");
	/* Verbatim content on a dim background bar, indented. */
	TEST_CHECK_STR_EQ(lines[1].text,
			  "  int x;");
	md_lines_free(lines, n);
}

static void test_code_fence_no_language(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("```\nplain\n```\nafter", 80, &n);
	TEST_CHECK_INT_EQ(n, 2);
	TEST_CHECK_STR_EQ(lines[0].text,
			  "  plain");
	TEST_CHECK_STR_EQ(lines[1].text, "after");
	md_lines_free(lines, n);
}

static void test_unterminated_fence(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("```python\nx = 1", 80, &n);
	dump_lines("unterminated_fence", lines, n);
	TEST_CHECK_INT_EQ(n, 2);
	TEST_CHECK_STR_EQ(lines[0].text, "  \033[0;2mpython\033[0m");
	TEST_CHECK_STR_EQ(lines[1].text,
			  "  x = 1");
	md_lines_free(lines, n);
}

static void test_code_fence_special_chars(void)
{
	/* Markdown syntax inside a fence stays verbatim. */
	size_t n = 0;
	md_line_t *lines = md_render("```\n**not bold** #not-heading\n```",
				     80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text,
			  "  **not bold** #not-heading");
	md_lines_free(lines, n);
}

static void test_lists(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("- a\n- b", 80, &n);
	TEST_CHECK_INT_EQ(n, 2);
	TEST_CHECK_STR_EQ(lines[0].text, "\xe2\x80\xa2 a");
	TEST_CHECK_STR_EQ(lines[1].text, "\xe2\x80\xa2 b");
	md_lines_free(lines, n);

	n = 0;
	lines = md_render("1. one\n2. two\n10. ten", 80, &n);
	TEST_CHECK_INT_EQ(n, 3);
	TEST_CHECK_STR_EQ(lines[0].text, "1. one");
	TEST_CHECK_STR_EQ(lines[1].text, "2. two");
	TEST_CHECK_STR_EQ(lines[2].text, "10. ten");
	md_lines_free(lines, n);

	/* Nested unordered item. */
	n = 0;
	lines = md_render("  - x", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "  \xe2\x80\xa2 x");
	md_lines_free(lines, n);
}

static void test_list_item_wraps(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("- abcdefghij klm", 10, &n);
	dump_lines("list_wrap", lines, n);
	TEST_CHECK(n >= 2);
	TEST_CHECK_STR_EQ(lines[0].text, "\xe2\x80\xa2 abcdefgh");
	/* Continuation lines carry a hanging indent matching the marker. */
	TEST_CHECK_INT_EQ(disp_width(lines[1].text), 8);
	TEST_CHECK(lines[1].text[0] == ' ');
	TEST_CHECK(lines[1].text[1] == ' ');
	for (size_t i = 0; i < n; i++) {
		TEST_CHECK(disp_width(lines[i].text) <= 10);
	}
	md_lines_free(lines, n);
}

static void test_blockquote(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("> hi", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "\033[0;2m\xe2\x94\x82\033[0m hi");
	md_lines_free(lines, n);

	n = 0;
	lines = md_render(">> deep", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text,
			  "\033[0;2m\xe2\x94\x82\033[0m \033[0;2m\xe2\x94\x82\033[0m deep");
	md_lines_free(lines, n);

	/* Bar-only quote line (prefix includes the separator space). */
	n = 0;
	lines = md_render(">", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "\033[0;2m\xe2\x94\x82\033[0m ");
	md_lines_free(lines, n);
}

static void test_links(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("see [text](http://example.com) now",
				     80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text,
			  "see \033[0;4mtext\033[0;2m "
			  "(http://example.com)\033[0m now");
	md_lines_free(lines, n);

	/* Malformed links stay literal. */
	n = 0;
	lines = md_render("[text](nope", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_STR_EQ(lines[0].text, "[text](nope");
	md_lines_free(lines, n);

	n = 0;
	lines = md_render("[text] (url)", 80, &n);
	TEST_CHECK_STR_EQ(lines[0].text, "[text] (url)");
	md_lines_free(lines, n);
}

static void test_horizontal_rule(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("---", 10, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_INT_EQ(disp_width(lines[0].text), 10);
	TEST_CHECK(strstr(lines[0].text, "\xe2\x94\x80") != NULL);
	md_lines_free(lines, n);

	n = 0;
	lines = md_render("***", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	TEST_CHECK_INT_EQ(disp_width(lines[0].text), 80);
	md_lines_free(lines, n);
}

static void test_blank_lines(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("a\n\nb", 80, &n);
	TEST_CHECK_INT_EQ(n, 3);
	TEST_CHECK_STR_EQ(lines[0].text, "a");
	TEST_CHECK_STR_EQ(lines[1].text, "");
	TEST_CHECK_STR_EQ(lines[2].text, "b");
	md_lines_free(lines, n);
}

static void test_word_wrap(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("The quick brown fox jumps", 10, &n);
	dump_lines("word_wrap", lines, n);
	TEST_CHECK_INT_EQ(n, 3);
	TEST_CHECK_STR_EQ(lines[0].text, "The quick");
	TEST_CHECK_STR_EQ(lines[1].text, "brown fox");
	TEST_CHECK_STR_EQ(lines[2].text, "jumps");
	for (size_t i = 0; i < n; i++) {
		TEST_CHECK(disp_width(lines[i].text) <= 10);
	}
	md_lines_free(lines, n);
}

static void test_hard_break_long_word(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("abcdefghij", 4, &n);
	TEST_CHECK_INT_EQ(n, 3);
	TEST_CHECK_STR_EQ(lines[0].text, "abcd");
	TEST_CHECK_STR_EQ(lines[1].text, "efgh");
	TEST_CHECK_STR_EQ(lines[2].text, "ij");
	md_lines_free(lines, n);
}

static void test_wrapping_never_exceeds_width(void)
{
	const char *text =
		"# Long heading that surely wraps around many times here\n"
		"Body **with styled** and `code` spans plus a [link](http://x.com/y) "
		"and a reallylongunbreakabletokenthatmustbehardwrappedtotest\n"
		"> quoted text that is quite long and needs wrapping too\n"
		"- list item with a long tail of words to force wrapping behavior\n"
		"```c\nint very_long_identifier_name_that_overflows_line = 123456;\n```\n";
	int widths[] = { 5, 9, 17, 24, 40, 80 };
	for (size_t wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
		int w = widths[wi];
		size_t n = 0;
		md_line_t *lines = md_render(text, w, &n);
		TEST_CHECK_NOT_NULL(lines);
		TEST_CHECK(n > 0);
		for (size_t i = 0; i < n; i++) {
			if (disp_width(lines[i].text) > w) {
				fprintf(stderr,
					"  FAIL width %d: line %zu is %d cols\n",
					w, i, disp_width(lines[i].text));
				test__failures++;
			}
		}
		md_lines_free(lines, n);
	}
}

static void test_control_chars_sanitized(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("a\x1b[31mred\tb\x07!", 80, &n);
	TEST_CHECK_INT_EQ(n, 1);
	/* ESC and BEL dropped, tab became a space. */
	TEST_CHECK_STR_EQ(lines[0].text, "a[31mred b!");
	md_lines_free(lines, n);
}

static void test_styles_reset_per_line(void)
{
	/* A styled heading followed by plain text: the second line must
	 * not inherit the heading style. */
	size_t n = 0;
	md_line_t *lines = md_render("# H\nplain", 80, &n);
	TEST_CHECK_INT_EQ(n, 2);
	TEST_CHECK_STR_EQ(lines[1].text, "plain");
	md_lines_free(lines, n);
}

static void test_empty_lines_only(void)
{
	size_t n = 0;
	md_line_t *lines = md_render("\n\n", 80, &n);
	TEST_CHECK_INT_EQ(n, 2);
	TEST_CHECK_STR_EQ(lines[0].text, "");
	TEST_CHECK_STR_EQ(lines[1].text, "");
	md_lines_free(lines, n);
}

int main(void)
{
	TEST_RUN(test_null_and_empty);
	TEST_RUN(test_plain_text);
	TEST_RUN(test_heading);
	TEST_RUN(test_bold_italic);
	TEST_RUN(test_inline_code);
	TEST_RUN(test_nested_styles);
	TEST_RUN(test_unclosed_constructs);
	TEST_RUN(test_code_fence);
	TEST_RUN(test_code_fence_no_language);
	TEST_RUN(test_unterminated_fence);
	TEST_RUN(test_code_fence_special_chars);
	TEST_RUN(test_lists);
	TEST_RUN(test_list_item_wraps);
	TEST_RUN(test_blockquote);
	TEST_RUN(test_links);
	TEST_RUN(test_horizontal_rule);
	TEST_RUN(test_blank_lines);
	TEST_RUN(test_word_wrap);
	TEST_RUN(test_hard_break_long_word);
	TEST_RUN(test_wrapping_never_exceeds_width);
	TEST_RUN(test_control_chars_sanitized);
	TEST_RUN(test_styles_reset_per_line);
	TEST_RUN(test_empty_lines_only);

	if (test__failures == 0)
		printf("All markdown tests passed\n");
	return test__failures ? 1 : 0;
}
