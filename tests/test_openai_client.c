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

#include <stdio.h>
#include <stdlib.h>

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

	cfg.api_key = (char *)"test-key-123";
	cfg.base_url = (char *)base_url;
	cfg.model_name = (char *)"test-model";
	cfg.temperature = 0.7;
	return cfg;
}

static void test_chat_completion_success(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[2];
	openai_response_t resp;

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
	openai_message_t msgs[1];
	openai_response_t resp;

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
	openai_message_t msgs[1];
	openai_response_t resp;

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
	openai_message_t msgs[1];
	openai_response_t resp;

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
	openai_message_t msgs[1];
	openai_response_t resp;

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
	openai_message_t msgs[1];
	openai_response_t resp;

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
	openai_message_t msgs[1];
	openai_response_t resp;

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
	openai_message_t msgs[1];
	openai_response_t resp;

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
	openai_message_t msgs[1];
	openai_response_t resp;

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

static void test_chat_completion_no_api_key(void)
{
	char base[128];
	openai_config_t cfg;
	openai_message_t msgs[1];
	openai_response_t resp;

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
	openai_message_t msgs[1];
	openai_response_t resp;

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

static void test_chat_completion_unreachable_server(void)
{
	openai_config_t cfg;
	openai_message_t msgs[1];
	openai_response_t resp;

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
	TEST_RUN(test_chat_completion_temperature_sent);
	TEST_RUN(test_chat_completion_no_api_key);
	TEST_RUN(test_chat_completion_unexpected_api_key);
	TEST_RUN(test_chat_completion_unreachable_server);

	http_client_cleanup();

	if (test__failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", test__failures);
		return 1;
	}
	printf("All openai_client/http_client tests passed\n");
	return 0;
}
