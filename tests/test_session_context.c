/**
 * @file test_session_context.c
 * @brief Unit tests for src/session_context.c (pure logic, no network).
 */

#include "test.h"
#include "session_context.h"

#include <stdlib.h>
#include <string.h>

static void test_init_success(void)
{
	session_context_t ctx;

	TEST_CHECK_INT_EQ(session_context_init(&ctx, 10), 0);
	TEST_CHECK_INT_EQ((long long)ctx.count, 0);
	TEST_CHECK_INT_EQ((long long)ctx.capacity, 4); /* INITIAL_CAPACITY */
	TEST_CHECK_INT_EQ((long long)ctx.max_messages, 10);
	TEST_CHECK_NOT_NULL(ctx.messages);
	session_context_free(&ctx);
	TEST_CHECK_NULL(ctx.messages); /* free() resets state */
	TEST_CHECK_INT_EQ((long long)ctx.count, 0);
}

static void test_init_null_context(void)
{
	TEST_CHECK_INT_EQ(session_context_init(NULL, 10), -1);
}

static void test_init_zero_max_messages(void)
{
	session_context_t ctx;

	/* A zero limit would truncate every message: must be rejected. */
	TEST_CHECK_INT_EQ(session_context_init(&ctx, 0), -1);
}

static void test_add_message_copies_strings(void)
{
	session_context_t ctx;
	char role[8];
	char content[32];

	strcpy(role, "user");
	strcpy(content, "hello");

	TEST_CHECK_INT_EQ(session_context_init(&ctx, 10), 0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, role, content), 0);

	/* Mutating the caller buffers must not affect the stored copies. */
	strcpy(content, "modified");
	strcpy(role, "system");

	TEST_CHECK_INT_EQ((long long)ctx.count, 1);
	TEST_CHECK_STR_EQ(ctx.messages[0].role, "user");
	TEST_CHECK_STR_EQ(ctx.messages[0].content, "hello");

	session_context_free(&ctx);
}

static void test_add_message_null_context(void)
{
	TEST_CHECK_INT_EQ(session_context_add_message(NULL, "user", "hi"), -1);
}

static void test_add_message_null_role(void)
{
	session_context_t ctx;

	TEST_CHECK_INT_EQ(session_context_init(&ctx, 10), 0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, NULL, "hi"), -1);
	session_context_free(&ctx);
}

static void test_add_message_null_content(void)
{
	session_context_t ctx;

	TEST_CHECK_INT_EQ(session_context_init(&ctx, 10), 0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "user", NULL), -1);
	session_context_free(&ctx);
}

static void test_dynamic_resizing(void)
{
	session_context_t ctx;
	size_t i;
	char label[16];

	TEST_CHECK_INT_EQ(session_context_init(&ctx, 100), 0);
	for (i = 0; i < 6; ++i) { /* more than INITIAL_CAPACITY (4) */
		snprintf(label, sizeof(label), "msg%zu", i);
		TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "user",
							      label), 0);
	}
	TEST_CHECK_INT_EQ((long long)ctx.count, 6);
	TEST_CHECK_INT_EQ((long long)ctx.capacity, 8); /* doubled once: 4 -> 8 */
	TEST_CHECK_STR_EQ(ctx.messages[0].content, "msg0");
	TEST_CHECK_STR_EQ(ctx.messages[5].content, "msg5");

	session_context_free(&ctx);
}

static void test_truncation_preserves_system_prompt(void)
{
	session_context_t ctx;

	TEST_CHECK_INT_EQ(session_context_init(&ctx, 2), 0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "system", "sys"),
			  0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "user", "u1"),
			  0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "user", "u2"),
			  0);

	/* Limit reached: the oldest non-system message ("u1") is dropped,
	 * the system prompt at the top is never evicted. */
	TEST_CHECK_INT_EQ((long long)ctx.count, 2);
	TEST_CHECK_STR_EQ(ctx.messages[0].role, "system");
	TEST_CHECK_STR_EQ(ctx.messages[0].content, "sys");
	TEST_CHECK_STR_EQ(ctx.messages[1].content, "u2");

	session_context_free(&ctx);
}

static void test_truncation_all_system_drops_oldest(void)
{
	session_context_t ctx;

	TEST_CHECK_INT_EQ(session_context_init(&ctx, 2), 0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "system", "s1"),
			  0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "system", "s2"),
			  0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "system", "s3"),
			  0);

	/* No non-system victim available: fall back to dropping the oldest. */
	TEST_CHECK_INT_EQ((long long)ctx.count, 2);
	TEST_CHECK_STR_EQ(ctx.messages[0].content, "s2");
	TEST_CHECK_STR_EQ(ctx.messages[1].content, "s3");

	session_context_free(&ctx);
}

static void test_truncation_with_single_message_limit(void)
{
	session_context_t ctx;

	TEST_CHECK_INT_EQ(session_context_init(&ctx, 1), 0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "user", "a"), 0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "user", "b"), 0);

	TEST_CHECK_INT_EQ((long long)ctx.count, 1);
	TEST_CHECK_STR_EQ(ctx.messages[0].content, "b");

	session_context_free(&ctx);
}

static void test_free_is_null_safe_and_idempotent(void)
{
	session_context_t ctx;

	session_context_free(NULL); /* must not crash */

	TEST_CHECK_INT_EQ(session_context_init(&ctx, 10), 0);
	TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "user", "x"), 0);

	session_context_free(&ctx);
	session_context_free(&ctx); /* second free must not crash */

	TEST_CHECK_NULL(ctx.messages);
	TEST_CHECK_INT_EQ((long long)ctx.count, 0);
	TEST_CHECK_INT_EQ((long long)ctx.capacity, 0);
	TEST_CHECK_INT_EQ((long long)ctx.max_messages, 0);
}

int main(void)
{
	TEST_RUN(test_init_success);
	TEST_RUN(test_init_null_context);
	TEST_RUN(test_init_zero_max_messages);
	TEST_RUN(test_add_message_copies_strings);
	TEST_RUN(test_add_message_null_context);
	TEST_RUN(test_add_message_null_role);
	TEST_RUN(test_add_message_null_content);
	TEST_RUN(test_dynamic_resizing);
	TEST_RUN(test_truncation_preserves_system_prompt);
	TEST_RUN(test_truncation_all_system_drops_oldest);
	TEST_RUN(test_truncation_with_single_message_limit);
	TEST_RUN(test_free_is_null_safe_and_idempotent);

	if (test__failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", test__failures);
		return 1;
	}
	printf("All session_context tests passed\n");
	return 0;
}
