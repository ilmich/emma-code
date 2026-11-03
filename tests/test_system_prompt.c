/**
 * @file test_system_prompt.c
 * @brief Unit tests for the built-in system prompt module.
 */

#include "test.h"
#include "system_prompt.h"

#include <string.h>

static void test_system_prompt_not_null(void)
{
	TEST_CHECK_NOT_NULL(system_prompt());
}

static void test_system_prompt_not_empty(void)
{
	const char *p = system_prompt();

	TEST_CHECK_NOT_NULL(p);
	TEST_CHECK(p[0] != '\0');
}

static void test_system_prompt_stable_pointer(void)
{
	/* Same pointer on repeated calls: the prompt is a static constant,
	 * which keeps the first request message byte-identical. */
	TEST_CHECK(system_prompt() == system_prompt());
}

static void test_system_prompt_content(void)
{
	const char *p = system_prompt();

	TEST_CHECK_NOT_NULL(strstr(p, "emma"));
	TEST_CHECK_NOT_NULL(strstr(p, "read_file"));
	TEST_CHECK_NOT_NULL(strstr(p, "list_dir"));
	TEST_CHECK_NOT_NULL(strstr(p, "write_file"));
	TEST_CHECK_NOT_NULL(strstr(p, "bash"));
	/* No trailing whitespace: the prompt must be byte-stable. */
	TEST_CHECK(p[strlen(p) - 1] != '\n');
	TEST_CHECK(p[strlen(p) - 1] != ' ');
}

int main(void)
{
	test_system_prompt_not_null();
	test_system_prompt_not_empty();
	test_system_prompt_stable_pointer();
	test_system_prompt_content();

	return test__failures != 0;
}
