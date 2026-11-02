/**
 * @file test_util.c
 * @brief Unit tests for src/util.c (util_strdup).
 */

#include "test.h"

#include "util.h"

#include <stdlib.h>

#include <string.h>

static void test_strdup_basic(void)
{
	char *copy = util_strdup("hello");

	TEST_CHECK(copy != NULL);
	TEST_CHECK_STR_EQ(copy, "hello");
	free(copy);
}

static void test_strdup_empty(void)
{
	char *copy = util_strdup("");

	TEST_CHECK(copy != NULL);
	TEST_CHECK(copy[0] == '\0');
	free(copy);
}

static void test_strdup_null_safe(void)
{
	TEST_CHECK(util_strdup(NULL) == NULL);
}

int main(void)
{
	TEST_RUN(test_strdup_basic);
	TEST_RUN(test_strdup_empty);
	TEST_RUN(test_strdup_null_safe);

	if (test__failures == 0)
		printf("All util tests passed\n");
	return test__failures ? 1 : 0;
}
