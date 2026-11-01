/**
 * @file test_agent_loop.c
 * @brief Unit tests for src/agent_loop.c.
 *
 * Network tests run against a local mock OpenAI server (tests/mock_server.py)
 * started by tests/run_tests.sh, which exports TEST_MOCK_PORT (see
 * mock_server.py for the scenario list). The TUI is not needed: every
 * tui_*() function is NULL-safe, so tests pass a NULL tui pointer.
 *
 * Note: AGENT_MSG_EXIT is only reachable when session_context_add_message()
 * fails on allocation (OOM); a full session truncates instead of failing.
 * That path mirrors the historical `break` in main.c and is not reachable
 * from these tests without allocation-failure injection.
 */

#include "test.h"
#include "agent_loop.h"
#include "http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Portable strdup replacement: strdup is POSIX and is not declared under
 * strict C99 (tests must not define feature-test macros). */
static char *dup_str(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	if (d != NULL)
		memcpy(d, s, n);
	return d;
}

/* Builds "http://127.0.0.1:<port>/<scenario>" into buf.
 * Returns 0 on success, -1 if the port is missing or truncation occurred. */
static int build_url(char *buf, size_t size, const char *scenario)
{
	const char *port = getenv("TEST_MOCK_PORT");
	int n;

	if (port == NULL || port[0] == '\0') {
		fprintf(stderr, "  FAIL %s:%d: TEST_MOCK_PORT not set\n",
			__FILE__, __LINE__);
		return -1;
	}
	n = snprintf(buf, size, "http://127.0.0.1:%s/%s", port, scenario);
	if (n < 0 || (size_t)n >= size) {
		fprintf(stderr, "  FAIL %s:%d: url truncated\n", __FILE__,
			__LINE__);
		return -1;
	}
	return 0;
}

/* Initializes a loop context over an empty session and a NULL (safe) TUI.
 * Returns 0 on success. */
static int make_loop(agent_loop_t *loop, session_context_t *ctx,
		     openai_config_t *cfg, const char *base_url)
{
	cfg->api_key = (char *)"test-key-123";
	cfg->base_url = (char *)base_url;
	cfg->model_name = (char *)"test-model";
	cfg->temperature = 0.7;

	if (session_context_init(ctx, 10) != 0)
		return -1;
	loop->config = cfg;
	loop->session = ctx;
	loop->tui = NULL;
	return 0;
}

static void test_null_loop(void)
{
	char *input = dup_str("Hello");
	char *reply = NULL;

	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(NULL, input,
							      &reply),
			  (long long)AGENT_MSG_ERROR);
	TEST_CHECK_NULL(reply);

	/* Input is owned and freed by the module on every path: it must
	 * not be freed again here. */
	input = NULL;
}

static void test_null_input(void)
{
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;
	char *reply = (char *)"sentinel";

	if (session_context_init(&ctx, 10) != 0)
		return;

	loop.config = &cfg;
	loop.session = &ctx;
	loop.tui = NULL;
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, NULL,
							       &reply),
			  (long long)AGENT_MSG_ERROR);
	TEST_CHECK_NULL(reply);
	TEST_CHECK_INT_EQ((long long)ctx.count, 0);
	session_context_free(&ctx);
}

static void test_null_reply_out(void)
{
	char *input = dup_str("Hello");
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;

	if (session_context_init(&ctx, 10) != 0) {
		free(input);
		return;
	}
	loop.config = &cfg;
	loop.session = &ctx;
	loop.tui = NULL;
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       NULL),
			  (long long)AGENT_MSG_ERROR);
	TEST_CHECK_INT_EQ((long long)ctx.count, 0);
	session_context_free(&ctx);
}

static void test_null_config(void)
{
	char *input = dup_str("Hello");
	session_context_t ctx;
	agent_loop_t loop;
	char *reply = NULL;

	if (session_context_init(&ctx, 10) != 0) {
		free(input);
		return;
	}

	loop.config = NULL;
	loop.session = &ctx;
	loop.tui = NULL;
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       &reply),
			  (long long)AGENT_MSG_ERROR);
	TEST_CHECK_NULL(reply);
	TEST_CHECK_INT_EQ((long long)ctx.count, 0);
	session_context_free(&ctx);
	/* input was freed by the module: do not free it again. */
}

static void test_full_turn_success(void)
{
	char base[128];
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;
	char *reply = NULL;

	if (build_url(base, sizeof(base), "ok") != 0)
		return;
	if (make_loop(&loop, &ctx, &cfg, base) != 0)
		return;

	char *input = dup_str("Hello");
	TEST_CHECK_NOT_NULL(input);
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       &reply),
			  (long long)AGENT_MSG_OK);
	TEST_CHECK_STR_EQ(reply, "Hello from mock!");

	/* The user message must be stored in the history before the
	 * request; the assistant message is stored by the caller. */
	TEST_CHECK_INT_EQ((long long)ctx.count, 1);
	TEST_CHECK_STR_EQ(ctx.messages[0].role, "user");
	TEST_CHECK_STR_EQ(ctx.messages[0].content, "Hello");

	free(reply);
	session_context_free(&ctx);
}

static void test_unreachable_server(void)
{
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;
	char *reply = NULL;

	if (make_loop(&loop, &ctx, &cfg, "http://127.0.0.1:1/ok") != 0)
		return;

	char *input = dup_str("Hello");
	TEST_CHECK_NOT_NULL(input);
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       &reply),
			  (long long)AGENT_MSG_ERROR);
	TEST_CHECK_NULL(reply);

	/* The user message stays in the history even on failure, exactly
	 * like the previous inline implementation. */
	TEST_CHECK_INT_EQ((long long)ctx.count, 1);
	TEST_CHECK_STR_EQ(ctx.messages[0].role, "user");

	session_context_free(&ctx);
}

int main(void)
{
	TEST_CHECK_INT_EQ(http_client_init(), 0);

	TEST_RUN(test_null_loop);
	TEST_RUN(test_null_input);
	TEST_RUN(test_null_reply_out);
	TEST_RUN(test_null_config);
	TEST_RUN(test_full_turn_success);
	TEST_RUN(test_unreachable_server);

	http_client_cleanup();

	if (test__failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", test__failures);
		return 1;
	}
	printf("All agent_loop tests passed\n");
	return 0;
}
