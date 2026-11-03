/**
 * @file test_env_info.c
 * @brief Unit tests for the environment info block builder.
 */

#include "test.h"
#include "env_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct utsname;
extern char *getcwd(char *buf, size_t size);

static void test_env_info_build_not_null(void)
{
	char *info = env_info_build();

	TEST_CHECK_NOT_NULL(info);
	free(info);
}

static void test_env_info_contains_fields(void)
{
	char *info = env_info_build();

	TEST_CHECK_NOT_NULL(info);
	TEST_CHECK_NOT_NULL(strstr(info, "Environment:"));
	TEST_CHECK_NOT_NULL(strstr(info, "- OS: "));
	TEST_CHECK_NOT_NULL(strstr(info, "- Working directory: "));
	TEST_CHECK_NOT_NULL(strstr(info, "- Date: "));
	free(info);
}

static void test_env_info_cwd_matches(void)
{
	char *info = env_info_build();
	char cwd[4096];

	TEST_CHECK_NOT_NULL(info);
	if (getcwd(cwd, sizeof(cwd)) != NULL) {
		char line[4200];

		snprintf(line, sizeof(line), "- Working directory: %s\n", cwd);
		TEST_CHECK_NOT_NULL(strstr(info, line));
	}
	free(info);
}

static void test_env_info_date_format(void)
{
	char *info = env_info_build();
	const char *date;

	TEST_CHECK_NOT_NULL(info);
	date = strstr(info, "- Date: ");
	TEST_CHECK_NOT_NULL(date);
	if (date != NULL) {
		int y, mon, d, h, min;

		/* Expected: "- Date: YYYY-MM-DD HH:MM (local time)" */
		TEST_CHECK_INT_EQ(sscanf(date,
					 "- Date: %d-%d-%d %d:%d",
					 &y, &mon, &d, &h, &min), 5);
		TEST_CHECK_NOT_NULL(strstr(date, "(local time)"));
	}
	free(info);
}

static void test_env_info_nul_terminated(void)
{
	char *info = env_info_build();

	TEST_CHECK_NOT_NULL(info);
	if (info != NULL) {
		size_t len = strlen(info);

		/* Reachable NUL and sane trailing newline. */
		TEST_CHECK(len > 0);
		TEST_CHECK(info[len] == '\0');
		TEST_CHECK(info[len - 1] == '\n');
	}
	free(info);
}

static void test_env_info_independent_allocations(void)
{
	char *a = env_info_build();
	char *b = env_info_build();

	TEST_CHECK_NOT_NULL(a);
	TEST_CHECK_NOT_NULL(b);
	/* Distinct heap blocks: no aliasing, no double-free risk. */
	TEST_CHECK(a != b);
	TEST_CHECK_STR_EQ(a, b);
	free(a);
	free(b);
}

int main(void)
{
	test_env_info_build_not_null();
	test_env_info_contains_fields();
	test_env_info_cwd_matches();
	test_env_info_date_format();
	test_env_info_nul_terminated();
	test_env_info_independent_allocations();

	return test__failures != 0;
}
