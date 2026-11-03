/**
 * @file test_system_prompt.c
 * @brief Unit tests for the built-in system prompt module.
 */

#include "test.h"
#include "system_prompt.h"

#include <stdlib.h>
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

static void test_system_prompt_workflow(void)
{
	const char *p = system_prompt();

	/* Explicit numbered workflow for non-reasoning models. */
	TEST_CHECK_NOT_NULL(strstr(p, "Workflow"));
	TEST_CHECK_NOT_NULL(strstr(p, "1. EXPLORE"));
	TEST_CHECK_NOT_NULL(strstr(p, "2. LOCATE"));
	TEST_CHECK_NOT_NULL(strstr(p, "3. CHANGE"));
	TEST_CHECK_NOT_NULL(strstr(p, "4. VERIFY"));
}

static void test_system_prompt_hard_rules(void)
{
	const char *p = system_prompt();

	/* Categorical prohibitions. */
	TEST_CHECK_NOT_NULL(strstr(p, "Hard rules:"));
	TEST_CHECK_NOT_NULL(strstr(p, "NEVER answer a question about the code"));
	TEST_CHECK_NOT_NULL(strstr(p, "NEVER invent file names"));
	TEST_CHECK_NOT_NULL(strstr(p, "NEVER create or overwrite a file"));
	/* Tool calls must be executed, not narrated in prose. */
	TEST_CHECK_NOT_NULL(strstr(p, "NEVER describe or narrate tool calls"));
	TEST_CHECK_NOT_NULL(strstr(p, "Keep prose short"));
}

static void test_system_prompt_full_prefix(void)
{
	char *full = system_prompt_full();
	const char *base = system_prompt();

	TEST_CHECK_NOT_NULL(full);
	if (full != NULL) {
		/* The full prompt starts with the exact static prompt. */
		TEST_CHECK(strncmp(full, base, strlen(base)) == 0);
	}
	free(full);
}

static void test_system_prompt_full_has_env_block(void)
{
	char *full = system_prompt_full();

	TEST_CHECK_NOT_NULL(full);
	if (full != NULL) {
		TEST_CHECK_NOT_NULL(strstr(full, "Environment:"));
		TEST_CHECK_NOT_NULL(strstr(full, "- Working directory: "));
	}
	free(full);
}

int main(void)
{
	test_system_prompt_not_null();
	test_system_prompt_not_empty();
	test_system_prompt_stable_pointer();
	test_system_prompt_content();
	test_system_prompt_workflow();
	test_system_prompt_hard_rules();
	test_system_prompt_full_prefix();
	test_system_prompt_full_has_env_block();

	return test__failures != 0;
}
