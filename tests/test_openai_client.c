/**
 * @file test_openai_client.c
 * @brief Unit tests for src/openai_client.c and src/http_client.c.
 *
 * Network tests run against a local mock OpenAI server (tests/mock_server.py)
 * started by tests/run_tests.sh, which exports TEST_MOCK_PORT. Scenarios are
 * selected through the first path segment of config->base_url, e.g.
 * "http://127.0.0.1:<port>/status500" (see mock_server.py for the list).
 */

#include "test.h"
#include "http_client.h"
#include "openai_client.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static openai_config_t make_config(const char *base_url)
{
	openai_config_t cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.api_key = (char *)"test-key-123";
	cfg.base_url = (char *)base_url;
	cfg.model_name = (char *)"test-model";
	cfg.temperature = 0.7;
	cfg.stream = 0;
	return cfg;
}

/* Delta collector for openai_chat_completion_stream tests, passed through
 * user_data. Records content and reasoning deltas in separate buffers plus
 * per-kind call counts. */
static struct delta_collector {
	char buf[256];
	size_t len;
	size_t count;
	char reasoning_buf[256];
	size_t reasoning_len;
	size_t reasoning_count;
} g_deltas;

static void delta_collect(const char *text, int is_reasoning, void *user_data)
{
	struct delta_collector *col = user_data;
	size_t n = strlen(text);

	if (is_reasoning) {
		if (col->reasoning_len + n < sizeof(col->reasoning_buf)) {
			memcpy(col->reasoning_buf + col->reasoning_len, text, n);
			col->reasoning_len += n;
			col->reasoning_buf[col->reasoning_len] = '\0';
			col->reasoning_count++;
		} else {
			test__failures++; /* collector overflow: report as failure */
			fprintf(stderr,
				"  FAIL %s:%d: reasoning collector overflow\n",
				__FILE__, __LINE__);
		}
		return;
	}
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

static openai_message_t make_user_message(void)
{
	openai_message_t msg;

	memset(&msg, 0, sizeof(msg));
	msg.role = (char *)"user";
	msg.content = (char *)"Hello";
	return msg;
}

static void test_chat_completion_success(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[2] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "ok") != 0) {
		return;
	}
	cfg = make_config(base);
	msgs[0].role = (char *)"system";
	msgs[0].content = (char *)"You are a helpful assistant.";
	msgs[1].role = (char *)"user";
	msgs[1].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 2, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");

	/* free() must release the content and reset the pointer. */
	openai_response_free(&resp);
	TEST_CHECK_NULL(resp.content);

	/* Second free must be safe. */
	openai_response_free(&resp);
}

static void test_chat_completion_null_model_name(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "ok") != 0) {
		return;
	}
	cfg = make_config(base);
	cfg.model_name = NULL;
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 1);
	openai_response_free(&resp);
}

static void test_chat_completion_null_message_fields(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "ok") != 0) {
		return;
	}
	cfg = make_config(base);
	msgs[0].role = NULL;
	msgs[0].content = NULL;
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 1);
	openai_response_free(&resp);
}

static void test_chat_completion_unauthorized(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "noauth") != 0) {
		return;
	}
	cfg = make_config(base);
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 1);
	openai_response_free(&resp);
}

static void test_chat_completion_server_error_500(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "status500") != 0) {
		return;
	}
	cfg = make_config(base);
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 1);
	openai_response_free(&resp);
}

static void test_chat_completion_malformed_json_body(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "badjson") != 0) {
		return;
	}
	cfg = make_config(base);
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 1);
	openai_response_free(&resp);
}

static void test_chat_completion_empty_choices(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "emptychoices") != 0) {
		return;
	}
	cfg = make_config(base);
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 1);
	openai_response_free(&resp);
}

static void test_chat_completion_missing_content(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "nocontent") != 0) {
		return;
	}
	cfg = make_config(base);
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 1);
	openai_response_free(&resp);
}

static void test_chat_completion_temperature_sent(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "temp0.42") != 0) {
		return;
	}
	/* The mock server rejects the request with 400 unless the JSON
	 * payload carries "temperature": 0.42. */
	cfg = make_config(base);
	cfg.temperature = 0.42;
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	openai_response_free(&resp);
}

static void test_chat_completion_system_message_first(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[2] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "sysfirst") != 0) {
		return;
	}
	/* The mock server rejects the request with 400 unless the first
	 * message of the payload is a non-empty system message: proves the
	 * client serializes messages in the given order, so a system prompt
	 * seeded as the first session message stays the cache-friendly
	 * prefix of every request. */
	cfg = make_config(base);
	msgs[0].role = (char *)"system";
	msgs[0].content = (char *)"You are emma.";
	msgs[1].role = (char *)"user";
	msgs[1].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 2, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	openai_response_free(&resp);
}

static void test_chat_completion_no_api_key(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "noauthkey") != 0) {
		return;
	}
	/* The mock server rejects requests that carry an Authorization
	 * header, so a NULL api_key must result in no header at all. */
	cfg = make_config(base);
	cfg.api_key = NULL;
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	openai_response_free(&resp);
}

static void test_chat_completion_unexpected_api_key(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	/* Converse of test_chat_completion_no_api_key: proves the mock
	 * really checks for the absence of the Authorization header. */
	if (build_url(base, sizeof(base), "noauthkey") != 0) {
		return;
	}
	cfg = make_config(base);
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 1);
	openai_response_free(&resp);
}

static void test_stream_success(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "aistream") != 0) {
		return;
	}
	cfg = make_config(base);
	cfg.stream = 1;
	msgs[0] = make_user_message();
	resp.content = NULL;
	memset(&g_deltas, 0, sizeof(g_deltas));

	TEST_CHECK_INT_EQ(openai_chat_completion_stream(&cfg, msgs, 1,
							delta_collect,
							&g_deltas, &resp), 0);
	/* The full accumulated text must equal the concatenation of the
	 * deltas; metadata-only and malformed chunks are ignored. */
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	TEST_CHECK_STR_EQ(g_deltas.buf, "Hello from mock!");
	TEST_CHECK_INT_EQ((long long)g_deltas.count, 3);

	openai_response_free(&resp);
	TEST_CHECK_NULL(resp.content);
}

static void test_stream_no_callback(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "aistream") != 0) {
		return;
	}
	cfg = make_config(base);
	cfg.stream = 1;
	msgs[0] = make_user_message();
	resp.content = NULL;

	/* on_delta = NULL must still produce the accumulated full text. */
	TEST_CHECK_INT_EQ(openai_chat_completion_stream(&cfg, msgs, 1, NULL,
							NULL, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	openai_response_free(&resp);
}

static void test_stream_fallback_non_sse(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "ok") != 0) {
		return;
	}
	cfg = make_config(base);
	cfg.stream = 1;
	msgs[0] = make_user_message();
	resp.content = NULL;
	memset(&g_deltas, 0, sizeof(g_deltas));

	/* The "ok" scenario answers with a plain JSON body even when
	 * "stream": true is requested: the client must fall back to the
	 * non-streaming parser and never call on_delta. */
	TEST_CHECK_INT_EQ(openai_chat_completion_stream(&cfg, msgs, 1,
							delta_collect,
							&g_deltas, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	TEST_CHECK_INT_EQ((long long)g_deltas.count, 0);
	openai_response_free(&resp);
}

static void test_stream_server_error_500(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "status500") != 0) {
		return;
	}
	cfg = make_config(base);
	cfg.stream = 1;
	msgs[0] = make_user_message();
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion_stream(&cfg, msgs, 1, NULL,
							NULL, &resp), 1);
	openai_response_free(&resp);
}

static void test_stream_unreachable_server(void)
{
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	/* Port 1 on localhost: connection refused, no timeout wait. */
	cfg.api_key = (char *)"test-key-123";
	cfg.base_url = (char *)"http://127.0.0.1:1/ok";
	cfg.model_name = (char *)"test-model";
	cfg.temperature = 0.7;
	cfg.stream = 1;
	msgs[0] = make_user_message();
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion_stream(&cfg, msgs, 1, NULL,
							NULL, &resp), 1);
	openai_response_free(&resp);
}

static void test_chat_completion_unreachable_server(void)
{
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	/* Port 1 on localhost: connection refused, no timeout wait. */
	cfg.api_key = (char *)"test-key-123";
	cfg.base_url = (char *)"http://127.0.0.1:1/ok";
	cfg.model_name = (char *)"test-model";
	cfg.temperature = 0.7;
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"Hello";
	resp.content = NULL;

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 1);
	openai_response_free(&resp);
}

static void test_tool_calls_parsed_non_streaming(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "toolcall") != 0)
		return;
	cfg = make_config(base);
	msgs[0] = make_user_message();

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 0);
	TEST_CHECK_NULL(resp.content);
	TEST_CHECK_INT_EQ((long long)resp.tool_call_count, 1);
	TEST_CHECK_STR_EQ(resp.tool_calls[0].id, "call_mock_1");
	TEST_CHECK_STR_EQ(resp.tool_calls[0].name, "read_file");
	TEST_CHECK_STR_EQ(resp.tool_calls[0].arguments,
			 "{\"path\": \"/tmp/emma-test.txt\"}");
	openai_response_free(&resp);
	TEST_CHECK_NULL(resp.tool_calls);
}

static void test_tool_calls_accumulated_streaming(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "ssetool") != 0)
		return;
	cfg = make_config(base);
	cfg.stream = 1;
	msgs[0] = make_user_message();

	TEST_CHECK_INT_EQ(
	    openai_chat_completion_stream(&cfg, msgs, 1, NULL, NULL, &resp),
	    0);
	TEST_CHECK_STR_EQ(resp.content, "");
	TEST_CHECK_INT_EQ((long long)resp.tool_call_count, 1);
	TEST_CHECK_STR_EQ(resp.tool_calls[0].id, "call_mock_9");
	TEST_CHECK_STR_EQ(resp.tool_calls[0].name, "read_file");
	/* The two argument fragments must be concatenated. */
	TEST_CHECK_STR_EQ(resp.tool_calls[0].arguments,
			 "{\"path\":\"/tmp/x\"}");
	openai_response_free(&resp);
}

static void test_tools_json_sent(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "toolreq") != 0)
		return;
	cfg = make_config(base);
	cfg.tools_json = (char *)"[{\"type\":\"function\"," 
	    "\"function\":{\"name\":\"read_file\"}}]";
	msgs[0] = make_user_message();

	/* The mock answers 200 only when the payload carries a non-empty
	 * "tools" array. */
	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	openai_response_free(&resp);

	/* Without tools_json the same scenario must fail (400). */
	cfg.tools_json = NULL;
	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 1);
	openai_response_free(&resp);
}

static void test_tool_messages_serialized(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[3] = {0};
	openai_tool_call_t calls[1];
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "toolround") != 0)
		return;
	cfg = make_config(base);

	/* user -> assistant (tool call) -> tool result: the mock returns the
	 * final text only when the tool result message round-trips. */
	msgs[0].role = (char *)"user";
	msgs[0].content = (char *)"read the file";
	calls[0].id = (char *)"call_mock_1";
	calls[0].name = (char *)"read_file";
	calls[0].arguments = (char *)"{}";
	msgs[1].role = (char *)"assistant";
	msgs[1].content = NULL;
	msgs[1].tool_calls = calls;
	msgs[1].tool_call_count = 1;
	msgs[2].role = (char *)"tool";
	msgs[2].content = (char *)"file body";
	msgs[2].tool_call_id = (char *)"call_mock_1";

	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 3, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	openai_response_free(&resp);
}

static void test_chat_completion_reasoning(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "reasoning") != 0) {
		return;
	}
	cfg = make_config(base);
	msgs[0] = make_user_message();

	/* message.reasoning_content must land in resp.reasoning. */
	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	TEST_CHECK_STR_EQ(resp.reasoning, "Thinking hard... then done.");
	openai_response_free(&resp);
	TEST_CHECK_NULL(resp.reasoning);
}

static void test_chat_completion_reasoning_alt_field(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "reasoningalt") != 0) {
		return;
	}
	cfg = make_config(base);
	msgs[0] = make_user_message();

	/* The alternative "reasoning" spelling must be parsed too. */
	TEST_CHECK_INT_EQ(openai_chat_completion(&cfg, msgs, 1, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	TEST_CHECK_STR_EQ(resp.reasoning, "Thinking hard... then done.");
	openai_response_free(&resp);
	TEST_CHECK_NULL(resp.reasoning);
}

static void test_stream_reasoning(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "ssereasoning") != 0) {
		return;
	}
	cfg = make_config(base);
	cfg.stream = 1;
	msgs[0] = make_user_message();
	memset(&g_deltas, 0, sizeof(g_deltas));

	TEST_CHECK_INT_EQ(openai_chat_completion_stream(&cfg, msgs, 1,
						delta_collect,
						&g_deltas, &resp), 0);
	/* Reasoning deltas must be accumulated separately from content. */
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	TEST_CHECK_STR_EQ(resp.reasoning, "Step one. Step two.");
	TEST_CHECK_STR_EQ(g_deltas.buf, "Hello from mock!");
	TEST_CHECK_STR_EQ(g_deltas.reasoning_buf, "Step one. Step two.");
	TEST_CHECK_INT_EQ((long long)g_deltas.count, 3);
	TEST_CHECK_INT_EQ((long long)g_deltas.reasoning_count, 2);

	openai_response_free(&resp);
	TEST_CHECK_NULL(resp.reasoning);
}

static void test_stream_no_reasoning_field(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1] = {0};
	openai_response_t resp = {0};

	if (build_url(base, sizeof(base), "aistream") != 0) {
		return;
	}
	cfg = make_config(base);
	cfg.stream = 1;
	msgs[0] = make_user_message();

	/* A stream without reasoning deltas must leave reasoning NULL. */
	TEST_CHECK_INT_EQ(openai_chat_completion_stream(&cfg, msgs, 1, NULL,
						NULL, &resp), 0);
	TEST_CHECK_STR_EQ(resp.content, "Hello from mock!");
	TEST_CHECK_NULL(resp.reasoning);
	openai_response_free(&resp);
}

static void test_message_free_null_safe(void)
{
	openai_message_t msg = {0};

	openai_message_free(NULL); /* must not crash */
	openai_message_free(&msg); /* all fields NULL: must not crash */

	msg.role = util_strdup("assistant");
	msg.content = util_strdup("text");
	msg.tool_call_id = util_strdup("call_1");
	msg.tool_calls = (openai_tool_call_t *)malloc(sizeof(*msg.tool_calls));
	TEST_CHECK_NOT_NULL(msg.tool_calls);
	msg.tool_calls[0].id = util_strdup("call_1");
	msg.tool_calls[0].name = util_strdup("bash");
	msg.tool_calls[0].arguments = util_strdup("{}");
	msg.tool_call_count = 1;
	openai_message_free(&msg);
	TEST_CHECK_NULL(msg.role);
	TEST_CHECK_NULL(msg.content);
	TEST_CHECK_NULL(msg.tool_call_id);
	TEST_CHECK_NULL(msg.tool_calls);
	TEST_CHECK_INT_EQ((long long)msg.tool_call_count, 0);
	openai_message_free(&msg); /* idempotent */
}

int main(void)
{
	TEST_CHECK_INT_EQ(http_client_init(), 0);

	TEST_RUN(test_chat_completion_success);
	TEST_RUN(test_chat_completion_null_model_name);
	TEST_RUN(test_chat_completion_null_message_fields);
	TEST_RUN(test_chat_completion_unauthorized);
	TEST_RUN(test_chat_completion_server_error_500);
	TEST_RUN(test_chat_completion_malformed_json_body);
	TEST_RUN(test_chat_completion_empty_choices);
	TEST_RUN(test_chat_completion_missing_content);
	TEST_RUN(test_chat_completion_system_message_first);
	TEST_RUN(test_chat_completion_temperature_sent);
	TEST_RUN(test_chat_completion_no_api_key);
	TEST_RUN(test_chat_completion_unexpected_api_key);
	TEST_RUN(test_chat_completion_unreachable_server);
	TEST_RUN(test_tool_calls_parsed_non_streaming);
	TEST_RUN(test_tool_calls_accumulated_streaming);
	TEST_RUN(test_tools_json_sent);
	TEST_RUN(test_tool_messages_serialized);
	TEST_RUN(test_message_free_null_safe);
	TEST_RUN(test_chat_completion_reasoning);
	TEST_RUN(test_chat_completion_reasoning_alt_field);

	TEST_RUN(test_stream_success);
	TEST_RUN(test_stream_no_callback);
	TEST_RUN(test_stream_reasoning);
	TEST_RUN(test_stream_no_reasoning_field);
	TEST_RUN(test_stream_fallback_non_sse);
	TEST_RUN(test_stream_server_error_500);
	TEST_RUN(test_stream_unreachable_server);

	http_client_cleanup();

	if (test__failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", test__failures);
		return 1;
	}
	printf("All openai_client/http_client tests passed\n");
	return 0;
}
