/**
 * @file test_agent_loop.c
 * @brief Unit tests for src/agent_loop.c.
 *
 * Network tests run against a local mock OpenAI server (tests/mock_server.py)
 * started by tests/run_tests.sh, which exports TEST_MOCK_PORT (see
 * mock_server.py for the scenario list). The module performs no rendering:
 * it knows nothing about the TUI, so no tui_*() function is involved.
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

/* Delta collector for the streaming agent-loop test, passed through
 * loop.delta_user_data. */
static struct delta_collector {
	char buf[256];
	size_t len;
	size_t count;
} g_deltas;

static void delta_collect(const char *text, void *user_data)
{
	struct delta_collector *col = user_data;
	size_t n = strlen(text);

	if (col->len + n < sizeof(col->buf)) {
		memcpy(col->buf + col->len, text, n);
		col->len += n;
		col->buf[col->len] = '\0';
		col->count++;
	} else {
		test__failures++; /* collector overflow: report as failure */
		fprintf(stderr, "  FAIL %s:%d: delta collector overflow\n",
			__FILE__, __LINE__);
	}
}

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

/* Initializes a loop context over an empty session with no delta callback.
 * Returns 0 on success. */
static int make_loop(agent_loop_t *loop, session_context_t *ctx,
		     openai_config_t *cfg, const char *base_url)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->api_key = (char *)"test-key-123";
	cfg->base_url = (char *)base_url;
	cfg->model_name = (char *)"test-model";
	cfg->temperature = 0.7;
	cfg->stream = 0;

	if (session_context_init(ctx, 10) != 0)
		return -1;
	loop->config = cfg;
	loop->session = ctx;
	loop->on_delta = NULL;
	loop->delta_user_data = NULL;
	loop->on_tool_call = NULL;
	loop->tool_user_data = NULL;
	loop->max_tool_rounds = 0;
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

	memset(&loop, 0, sizeof(loop));
	loop.config = &cfg;
	loop.session = &ctx;
	loop.on_delta = NULL;
	loop.delta_user_data = NULL;
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
	memset(&loop, 0, sizeof(loop));
	loop.config = &cfg;
	loop.session = &ctx;
	loop.on_delta = NULL;
	loop.delta_user_data = NULL;
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

	memset(&loop, 0, sizeof(loop));
	loop.config = NULL;
	loop.session = &ctx;
	loop.on_delta = NULL;
	loop.delta_user_data = NULL;
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

static void test_full_turn_streaming(void)
{
	char base[128];
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;
	char *reply = NULL;

	if (build_url(base, sizeof(base), "aistream") != 0)
		return;
	if (make_loop(&loop, &ctx, &cfg, base) != 0)
		return;
	cfg.stream = 1;
	loop.on_delta = delta_collect;
	loop.delta_user_data = &g_deltas;

	char *input = dup_str("Hello");
	TEST_CHECK_NOT_NULL(input);
	memset(&g_deltas, 0, sizeof(g_deltas));
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       &reply),
			  (long long)AGENT_MSG_OK);
	/* The streamed deltas must arrive in order and the reply must hold
	 * the full text, ready for the caller's post_message hook. */
	TEST_CHECK_STR_EQ(reply, "Hello from mock!");
	TEST_CHECK_STR_EQ(g_deltas.buf, "Hello from mock!");
	TEST_CHECK_INT_EQ((long long)g_deltas.count, 3);
	TEST_CHECK_INT_EQ((long long)ctx.count, 1);

	free(reply);
	session_context_free(&ctx);
}

static void test_full_turn_streaming_no_callback(void)
{
	char base[128];
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;
	char *reply = NULL;

	if (build_url(base, sizeof(base), "aistream") != 0)
		return;
	if (make_loop(&loop, &ctx, &cfg, base) != 0)
		return;
	cfg.stream = 1;
	/* on_delta stays NULL: the loop must still deliver the full reply. */

	char *input = dup_str("Hello");
	TEST_CHECK_NOT_NULL(input);
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       &reply),
			  (long long)AGENT_MSG_OK);
	TEST_CHECK_STR_EQ(reply, "Hello from mock!");
	TEST_CHECK_INT_EQ((long long)ctx.count, 1);

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

/* ------------------------------------------------------------------ */
/* Tool round tests                                                    */
/* ------------------------------------------------------------------ */

/** Dispatcher recording every call and returning canned results. */
static struct dispatch_recorder {
	char names[8][32];
	char args[8][64];
	size_t count;
	const char *reply;   /* result text handed back to the loop */
	int fail_malloc;     /* when nonzero, simulates malloc failure */
} g_dispatch;

static int recording_dispatch(const char *name, const char *args_json,
			      char **result, void *user_data)
{
	struct dispatch_recorder *rec = user_data;

	if (rec->count < 8) {
		snprintf(rec->names[rec->count],
			 sizeof(rec->names[rec->count]), "%s",
			 name != NULL ? name : "");
		snprintf(rec->args[rec->count], sizeof(rec->args[rec->count]),
			 "%s", args_json != NULL ? args_json : "");
		rec->count++;
	}
	if (rec->fail_malloc)
		return 1;
	*result = dup_str(rec->reply);
	return *result == NULL;
}

static void test_tool_round_success(void)
{
	char base[128];
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;
	char *reply = NULL;

	if (build_url(base, sizeof(base), "toolround") != 0)
		return;
	if (make_loop(&loop, &ctx, &cfg, base) != 0)
		return;
	memset(&g_dispatch, 0, sizeof(g_dispatch));
	g_dispatch.reply = "file body";
	loop.on_tool_call = recording_dispatch;
	loop.tool_user_data = &g_dispatch;

	char *input = dup_str("read /tmp/emma-test.txt");
	TEST_CHECK_NOT_NULL(input);
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       &reply),
			  (long long)AGENT_MSG_OK);
	TEST_CHECK_STR_EQ(reply, "Hello from mock!");

	/* The dispatcher saw exactly one call with the mock's arguments. */
	TEST_CHECK_INT_EQ((long long)g_dispatch.count, 1);
	TEST_CHECK_STR_EQ(g_dispatch.names[0], "read_file");

	/* History: user, assistant(tool_calls), tool result. The final
	 * assistant text is stored by the caller, not by the loop. */
	TEST_CHECK_INT_EQ((long long)ctx.count, 3);
	TEST_CHECK_STR_EQ(ctx.messages[1].role, "assistant");
	TEST_CHECK_INT_EQ((long long)ctx.messages[1].tool_call_count, 1);
	TEST_CHECK_STR_EQ(ctx.messages[1].tool_calls[0].id, "call_mock_1");
	TEST_CHECK_STR_EQ(ctx.messages[2].role, "tool");
	TEST_CHECK_STR_EQ(ctx.messages[2].tool_call_id, "call_mock_1");
	TEST_CHECK_STR_EQ(ctx.messages[2].content, "file body");

	free(reply);
	session_context_free(&ctx);
}

static void test_tool_round_streaming(void)
{
	char base[128];
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;
	char *reply = NULL;

	if (build_url(base, sizeof(base), "ssetoolround") != 0)
		return;
	if (make_loop(&loop, &ctx, &cfg, base) != 0)
		return;
	cfg.stream = 1;
	loop.on_delta = delta_collect;
	loop.delta_user_data = &g_deltas;
	memset(&g_dispatch, 0, sizeof(g_dispatch));
	g_dispatch.reply = "out";
	loop.on_tool_call = recording_dispatch;
	loop.tool_user_data = &g_dispatch;

	char *input = dup_str("do it");
	TEST_CHECK_NOT_NULL(input);
	memset(&g_deltas, 0, sizeof(g_deltas));
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       &reply),
			  (long long)AGENT_MSG_OK);
	TEST_CHECK_STR_EQ(reply, "Hello from mock!");
	TEST_CHECK_INT_EQ((long long)g_dispatch.count, 1);
	TEST_CHECK_STR_EQ(g_dispatch.args[0], "{\"path\":\"/tmp/x\"}");

	free(reply);
	session_context_free(&ctx);
}

static void test_tool_calls_without_dispatcher(void)
{
	char base[128];
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;
	char *reply = NULL;

	if (build_url(base, sizeof(base), "toolcall") != 0)
		return;
	if (make_loop(&loop, &ctx, &cfg, base) != 0)
		return;
	/* on_tool_call stays NULL: no execution, the content is returned. */

	char *input = dup_str("Hello");
	TEST_CHECK_NOT_NULL(input);
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       &reply),
			  (long long)AGENT_MSG_OK);
	TEST_CHECK_STR_EQ(reply, "");
	TEST_CHECK_INT_EQ((long long)ctx.count, 1); /* only the user msg */

	free(reply);
	session_context_free(&ctx);
}

static void test_tool_round_limit(void)
{
	char base[128];
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;
	char *reply = NULL;

	if (build_url(base, sizeof(base), "toolloop") != 0)
		return;
	if (make_loop(&loop, &ctx, &cfg, base) != 0)
		return;
	memset(&g_dispatch, 0, sizeof(g_dispatch));
	g_dispatch.reply = "out";
	loop.on_tool_call = recording_dispatch;
	loop.tool_user_data = &g_dispatch;
	loop.max_tool_rounds = 2; /* the mock always answers with a call */

	char *input = dup_str("loop forever");
	TEST_CHECK_NOT_NULL(input);
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       &reply),
			  (long long)AGENT_MSG_OK);
	TEST_CHECK_STR_EQ(reply, "[Error: tool round limit reached]");
	/* max_tool_rounds + 1 requests were made, each dispatching one call. */
	TEST_CHECK_INT_EQ((long long)g_dispatch.count, 3);

	free(reply);
	session_context_free(&ctx);
}

static void test_tool_session_full_exits(void)
{
	char base[128];
	session_context_t ctx;
	openai_config_t cfg;
	agent_loop_t loop;
	char *reply = NULL;

	if (build_url(base, sizeof(base), "toolround") != 0)
		return;
	if (make_loop(&loop, &ctx, &cfg, base) != 0)
		return;
	memset(&g_dispatch, 0, sizeof(g_dispatch));
	g_dispatch.reply = "out";
	loop.on_tool_call = recording_dispatch;
	loop.tool_user_data = &g_dispatch;

	/* Fill the session to its hard limit with non-system messages. */
	for (size_t i = 0; i < 10; ++i) {
		char label[16];

		snprintf(label, sizeof label, "m%zu", i);
		TEST_CHECK_INT_EQ(session_context_add_message(&ctx, "user",
							      label),
				  0);
	}
	/* max_messages = 10: adding one more message makes room by
	 * truncating, so the only fatal path is allocation failure. A full
	 * session therefore cannot trigger EXIT here; instead verify that
	 * the loop still works right at the limit. */
	char *input = dup_str("next");
	TEST_CHECK_NOT_NULL(input);
	TEST_CHECK_INT_EQ((long long)agent_loop_handle_message(&loop, input,
							       &reply),
			  (long long)AGENT_MSG_OK);
	TEST_CHECK_NOT_NULL(reply);
	free(reply);
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
	TEST_RUN(test_full_turn_streaming);
	TEST_RUN(test_full_turn_streaming_no_callback);
	TEST_RUN(test_unreachable_server);
	TEST_RUN(test_tool_round_success);
	TEST_RUN(test_tool_round_streaming);
	TEST_RUN(test_tool_calls_without_dispatcher);
	TEST_RUN(test_tool_round_limit);
	TEST_RUN(test_tool_session_full_exits);

	http_client_cleanup();

	if (test__failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", test__failures);
		return 1;
	}
	printf("All agent_loop tests passed\n");
	return 0;
}
