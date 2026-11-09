/**
 * @file test_tui.c
 * @brief Unit tests for src/tui.c.
 *
 * The TUI needs a controlling terminal for raw mode, so full rendering is
 * not exercised here. These tests cover the NULL-safety of the public API
 * and the graceful-failure path of tui_init() when stdin is not a tty.
 * Rendering behavior (layout, cursor positioning) is verified manually or
 * through a pty harness.
 */

#include "test.h"

#include "tui.h"
#include "version.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BANNER_FULL_BLOCK "\xe2\x96\x88" /* U+2588 */

static void test_banner_lines(void)
{
	size_t count = 0;
	char **lines = tui_banner_lines(&count);

	TEST_CHECK_NOT_NULL(lines);
	TEST_CHECK_INT_EQ((long long)count, 6); /* 5 art rows + version */

	if (lines != NULL && count == 6) {
		/* The last line is exactly the version. */
		TEST_CHECK_STR_EQ(lines[5], "v" EMMA_VERSION);

		for (size_t i = 0; i < 5; i++) {
			const unsigned char *p =
				(const unsigned char *)lines[i];

			/* Every art row contains full blocks... */
			TEST_CHECK_NOT_NULL(strstr(lines[i],
						   BANNER_FULL_BLOCK));
			/* ...and only spaces, ASCII or the UTF-8 full
			 * block: no shadow/outline box-drawing
			 * characters (\xe2\x95\x90..\xe2\x95\xbf). */
			while (*p != 0) {
				if (*p == 0xe2) {
					TEST_CHECK_INT_EQ(p[1], 0x96);
					TEST_CHECK_INT_EQ(p[2], 0x88);
					p += 3;
				} else {
					TEST_CHECK(*p >= 0x20 && *p < 0x7f);
					p++;
				}
			}
			/* Fits an 80-column terminal (1 column per
			 * block char, 3 bytes per block char). */
			TEST_CHECK(strlen(lines[i]) <= 3 * 80);
		}
	}

	if (lines != NULL) {
		for (size_t i = 0; i < count; i++)
			free(lines[i]);
		free(lines);
	}
}

static void test_banner_lines_null_count_and_copies(void)
{
	size_t count = 0;
	char **a = tui_banner_lines(NULL); /* NULL out_count tolerated */
	char **b = tui_banner_lines(&count);

	TEST_CHECK_NOT_NULL(a);
	TEST_CHECK_NOT_NULL(b);
	TEST_CHECK_INT_EQ((long long)count, 6);

	if (a != NULL && b != NULL) {
		/* Independent allocations, identical content. */
		TEST_CHECK(a[0] != b[0]);
		TEST_CHECK_STR_EQ(a[0], b[0]);
		TEST_CHECK(a[5] != b[5]);
		TEST_CHECK_STR_EQ(a[5], b[5]);
	}

	if (a != NULL) {
		for (size_t i = 0; i < 6; i++)
			free(a[i]);
		free(a);
	}
	if (b != NULL) {
		for (size_t i = 0; i < count && i < 6; i++)
			free(b[i]);
		free(b);
	}
}

static void test_show_banner_null_safe(void)
{
	/* Must not crash. */
	tui_show_banner(NULL);
}

static void test_clear_null_safe(void)
{
	/* Must not crash. */
	tui_clear(NULL);
}

static void test_free_null_safe(void)
{
	/* NULL and double free must not crash. */
	tui_free(NULL);

	{
		tui_t *tui = NULL;

		tui_append_message(NULL, "user", "hi");
		tui_set_status(NULL, "busy");
		tui_free(tui);
	}
}

static void test_tool_block_null_safe(void)
{
	/* The tool block API is NULL-safe and rejects empty titles without
	 * crashing (full rendering is exercised under the pty harness). */
	tui_tool_start(NULL, "bash ls");
	tui_tool_start(NULL, NULL);
	tui_tool_start(NULL, "");
	tui_tool_finish(NULL, 1, "output");
	tui_tool_finish(NULL, 0, NULL);
	tui_tool_finish(NULL, 1, NULL);
}

static void test_api_null_args_safe(void)
{
	/* append_message/set_status/stream_append reject NULL args. */
	tui_append_message(NULL, NULL, "content");
	tui_append_message(NULL, "user", NULL);
	tui_set_status(NULL, NULL);
	tui_stream_append(NULL, NULL);
	tui_stream_append(NULL, "delta");
	tui_stream_reasoning_append(NULL, NULL);
	tui_stream_reasoning_append(NULL, "reasoning delta");
	tui_replace_assistant_message(NULL, NULL, NULL);
	tui_replace_assistant_message(NULL, "content", NULL);
	tui_replace_assistant_message(NULL, "content", "reasoning");

	/* tui_confirm denies on NULL arguments without crashing. */
	TEST_CHECK_INT_EQ((long long)tui_confirm(NULL, "proceed?"), 0);
	TEST_CHECK_INT_EQ((long long)tui_confirm(NULL, NULL), 0);
}

static void test_init_fails_without_tty(void)
{
	tui_t *tui = NULL;

	if (isatty(STDIN_FILENO)) {
		/* Running interactively: skip rather than hijack the
		 * developer's terminal. */
		fprintf(stderr, "  SKIP test_init_fails_without_tty: "
				"stdin is a tty\n");
		return;
	}
	/* NULL tui out-pointer is rejected without crashing. */
	TEST_CHECK_INT_EQ(tui_init(NULL, NULL), -1);

	/* Without a controlling terminal, entering raw mode must fail
	 * cleanly (no allocation returned, no crash). NULL theme selects
	 * the built-in default. */
	TEST_CHECK_INT_EQ(tui_init(&tui, NULL), -1);
	TEST_CHECK_NULL(tui);

	{
		/* An explicit default theme must behave the same way. */
		theme_t theme = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};

		TEST_CHECK_INT_EQ(theme_default(&theme), 0);
		tui = NULL;
		TEST_CHECK_INT_EQ(tui_init(&tui, &theme), -1);
		TEST_CHECK_NULL(tui);
		theme_free(&theme);
	}
}

static void test_input_line_col(void)
{
	int line = -1, col = -1;

	/* Empty buffer: everything is on line 0, column 0. */
	tui_input_line_col("", 0, 0, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 0);
	TEST_CHECK_INT_EQ(col, 0);

	/* Plain single-line text. */
	tui_input_line_col("abc", 3, 2, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 0);
	TEST_CHECK_INT_EQ(col, 2);
	tui_input_line_col("abc", 3, 3, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 0);
	TEST_CHECK_INT_EQ(col, 3);

	/* Newlines split lines; the index after the newline is line 1. */
	tui_input_line_col("a\nb", 3, 2, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 1);
	TEST_CHECK_INT_EQ(col, 0);
	tui_input_line_col("a\nb", 3, 3, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 1);
	TEST_CHECK_INT_EQ(col, 1);

	/* Trailing newline: the empty last line counts. */
	tui_input_line_col("a\n", 2, 2, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 1);
	TEST_CHECK_INT_EQ(col, 0);

	/* Wrapping: first line wraps at first_width, later lines at width. */
	tui_input_line_col("abcdefghij", 10, 5, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 1);
	TEST_CHECK_INT_EQ(col, 0);
	tui_input_line_col("abcdefghij", 10, 7, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 1);
	TEST_CHECK_INT_EQ(col, 2);
	tui_input_line_col("abcdefghij", 10, 10, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 1);
	TEST_CHECK_INT_EQ(col, 5);

	/* A newline right at the wrap boundary: the wrap starts a line and
	 * the newline starts another. */
	tui_input_line_col("abcde\nx", 7, 5, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 1);
	TEST_CHECK_INT_EQ(col, 0);
	tui_input_line_col("abcde\nx", 7, 6, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 2);
	TEST_CHECK_INT_EQ(col, 0);
	tui_input_line_col("abcde\nx", 7, 7, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 2);
	TEST_CHECK_INT_EQ(col, 1);

	/* Index clamped to len; NULL out-params tolerated. */
	tui_input_line_col("abc", 3, 99, 5, 10, NULL, NULL);
	tui_input_line_col("abc", 3, 99, 5, 10, &line, &col);
	TEST_CHECK_INT_EQ(line, 0);
	TEST_CHECK_INT_EQ(col, 3);
}

static void test_input_index_at(void)
{
	size_t idx = 999;

	/* Round-trips within a line. */
	TEST_CHECK_INT_EQ(tui_input_index_at("abc", 3, 5, 10, 0, 2, &idx), 0);
	TEST_CHECK_INT_EQ((long long)idx, 2);

	/* Column clamped to the line's end (before the newline). */
	TEST_CHECK_INT_EQ(tui_input_index_at("a\nb", 3, 5, 10, 0, 99, &idx), 0);
	TEST_CHECK_INT_EQ((long long)idx, 1);
	TEST_CHECK_INT_EQ(tui_input_index_at("a\nb", 3, 5, 10, 1, 0, &idx), 0);
	TEST_CHECK_INT_EQ((long long)idx, 2);
	TEST_CHECK_INT_EQ(tui_input_index_at("a\nb", 3, 5, 10, 1, 99, &idx), 0);
	TEST_CHECK_INT_EQ((long long)idx, 3);

	/* Wrap boundary: column == width lands on the next line. */
	TEST_CHECK_INT_EQ(tui_input_index_at("abcdefgh", 8, 5, 10, 1, 0, &idx),
			  0);
	TEST_CHECK_INT_EQ((long long)idx, 5);
	TEST_CHECK_INT_EQ(tui_input_index_at("abcdefgh", 8, 5, 10, 1, 5, &idx),
			  0);
	TEST_CHECK_INT_EQ((long long)idx, 10 > 8 ? 8 : 10); /* clamped to len */

	/* Empty last line (trailing newline) maps to len. */
	TEST_CHECK_INT_EQ(tui_input_index_at("a\n", 2, 5, 10, 1, 0, &idx), 0);
	TEST_CHECK_INT_EQ((long long)idx, 2);

	/* Out-of-range lines fail without touching the output. */
	idx = 123;
	TEST_CHECK_INT_EQ(tui_input_index_at("abc", 3, 5, 10, 1, 0, &idx), -1);
	TEST_CHECK_INT_EQ((long long)idx, 123);
	TEST_CHECK_INT_EQ(tui_input_index_at("abc", 3, 5, 10, -1, 0, &idx), -1);
	TEST_CHECK_INT_EQ(tui_input_index_at(NULL, 0, 5, 10, 0, 0, &idx), -1);
}

static void test_stream_emit_to(void)
{
	/* Nothing to emit for an empty or single-line message: the only
	 * line is the trailing one and stays in the live area. */
	TEST_CHECK_INT_EQ(tui_stream_emit_to(0, 0), -1);
	TEST_CHECK_INT_EQ(tui_stream_emit_to(1, 0), -1);

	/* Without flush: all lines but the trailing one are emitted. */
	TEST_CHECK_INT_EQ(tui_stream_emit_to(3, 0), 1);
	TEST_CHECK_INT_EQ(tui_stream_emit_to(5, 0), 3);

	/* With flush: everything, including the trailing line. */
	TEST_CHECK_INT_EQ(tui_stream_emit_to(1, 1), 0);
	TEST_CHECK_INT_EQ(tui_stream_emit_to(3, 1), 2);
}

int main(void)
{
	TEST_RUN(test_free_null_safe);
	TEST_RUN(test_tool_block_null_safe);
	TEST_RUN(test_api_null_args_safe);
	TEST_RUN(test_init_fails_without_tty);
	TEST_RUN(test_input_line_col);
	TEST_RUN(test_input_index_at);
	TEST_RUN(test_stream_emit_to);
	TEST_RUN(test_banner_lines);
	TEST_RUN(test_banner_lines_null_count_and_copies);
	TEST_RUN(test_show_banner_null_safe);
	TEST_RUN(test_clear_null_safe);

	if (test__failures == 0)
		printf("All tui tests passed\n");
	return test__failures ? 1 : 0;
}
