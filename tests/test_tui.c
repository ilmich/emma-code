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

#include <unistd.h>

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

static void test_api_null_args_safe(void)
{
	/* append_message/set_status/stream_append reject NULL args. */
	tui_append_message(NULL, NULL, "content");
	tui_append_message(NULL, "user", NULL);
	tui_set_status(NULL, NULL);
	tui_stream_append(NULL, NULL);
	tui_stream_append(NULL, "delta");
	tui_replace_assistant_message(NULL, NULL);
	tui_replace_assistant_message(NULL, "content");
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
	/* Without a controlling terminal, entering raw mode must fail
	 * cleanly (no allocation returned, no crash). */
	TEST_CHECK_INT_EQ(tui_init(&tui), -1);
	TEST_CHECK_NULL(tui);
}

int main(void)
{
	TEST_RUN(test_free_null_safe);
	TEST_RUN(test_api_null_args_safe);
	TEST_RUN(test_init_fails_without_tty);

	if (test__failures == 0)
		printf("All tui tests passed\n");
	return test__failures ? 1 : 0;
}
