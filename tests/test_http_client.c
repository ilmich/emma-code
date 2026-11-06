/**
 * @file test_http_client.c
 * @brief Unit tests for src/http_client.c.
 *
 * Tests run against the local mock server (tests/mock_server.py) started by
 * tests/run_tests.sh, which exports TEST_MOCK_PORT. See mock_server.py for
 * the scenario routing rules.
 */

#include "test.h"
#include "http_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Collector for streaming callbacks. The user_data parameter points to
 * this struct, passed through http_request_t.user_data. */
static struct sse_collector {
	char buf[256];
	size_t len;
} g_collector;

static void sse_collect(const char *buffer, size_t size, void *user_data)
{
	struct sse_collector *col = user_data;

	if (col == NULL)
		col = &g_collector; /* legacy: file-scope fallback */
	if (col->len + size < sizeof(col->buf)) {
		memcpy(col->buf + col->len, buffer, size);
		col->len += size;
		col->buf[col->len] = '\0';
	} else {
		test__failures++; /* collector overflow: report as failure */
		fprintf(stderr, "  FAIL %s:%d: sse collector overflow\n",
			__FILE__, __LINE__);
	}
}

/* Builds "http://127.0.0.1:<port>/<path>" into buf.
 * Returns 0 on success, -1 if the port is missing or truncation occurred. */
static int build_url(char *buf, size_t size, const char *path)
{
	const char *port = getenv("TEST_MOCK_PORT");
	int n;

	if (port == NULL || port[0] == '\0') {
		fprintf(stderr, "  FAIL %s:%d: TEST_MOCK_PORT not set\n",
			__FILE__, __LINE__);
		return -1;
	}
	n = snprintf(buf, size, "http://127.0.0.1:%s/%s", port, path);
	if (n < 0 || (size_t)n >= size) {
		fprintf(stderr, "  FAIL %s:%d: url truncated\n", __FILE__,
			__LINE__);
		return -1;
	}
	return 0;
}

static void test_http_request_post_success(void)
{
	char url[128];
	struct http_request_t req = {0};
	struct http_response_t resp = {0};
	char *headers[2] = {
		(char *)"Content-Type: application/json",
		(char *)"Authorization: Bearer test-key-123"
	};

	if (build_url(url, sizeof(url), "ok/direct") != 0) {
		return;
	}
	req.url = url;
	req.method = (char *)"POST";
	req.body = (char *)"{\"model\":\"test-model\",\"messages\":[]}";
	req.headers = headers;
	req.headers_count = 2;
	req.callback = NULL;
	req.user_data = NULL;

	TEST_CHECK_INT_EQ(http_request(&req, &resp), 0);
	TEST_CHECK_INT_EQ(resp.status_code, 200);
	TEST_CHECK_NOT_NULL(resp.body);
	TEST_CHECK_NOT_NULL(strstr(resp.body, "Hello from mock!"));

	http_response_free(&resp);
	TEST_CHECK_NULL(resp.body);
	TEST_CHECK_NULL(resp.headers);
}

static void test_http_request_nonexistent_scenario(void)
{
	char url[128];
	struct http_request_t req = {0};
	struct http_response_t resp = {0};

	if (build_url(url, sizeof(url), "unknown-scenario") != 0) {
		return;
	}
	req.url = url;
	req.method = (char *)"GET";

	TEST_CHECK_INT_EQ(http_request(&req, &resp), 0);
	TEST_CHECK_INT_EQ(resp.status_code, 404);

	http_response_free(&resp);
}

static void test_http_request_unreachable(void)
{
	struct http_request_t req = {0};
	struct http_response_t resp = {0};

	/* Port 1 on localhost: connection refused, no timeout wait. */
	req.url = (char *)"http://127.0.0.1:1/ok";
	req.method = (char *)"GET";

	TEST_CHECK_INT_EQ(http_request(&req, &resp), 1);
	http_response_free(&resp); /* must tolerate an unfilled response */
}

/* Regression: CRLF-separated SSE events (\r\n\r\n) must be dispatched
 * exactly like LF-separated ones, and the trailing \r must be stripped. */
static void test_http_stream_sse_crlf(void)
{
	char url[128];
	struct http_request_t req = {0};
	struct http_response_t resp = {0};

	if (build_url(url, sizeof(url), "ssecrlf") != 0) {
		return;
	}
	req.url = url;
	req.method = (char *)"GET";
	req.callback = sse_collect;
	req.user_data = &g_collector;

	memset(&g_collector, 0, sizeof(g_collector));

	TEST_CHECK_INT_EQ(http_request(&req, &resp), 0);
	TEST_CHECK_INT_EQ(resp.status_code, 200);
	TEST_CHECK_STR_EQ(g_collector.buf, "firstsecond");

	http_response_free(&resp);
}

/* Regression: "data:" without a space after the colon is spec-legal. */
static void test_http_stream_sse_no_space(void)
{
	char url[128];
	struct http_request_t req = {0};
	struct http_response_t resp = {0};

	if (build_url(url, sizeof(url), "ssespace") != 0) {
		return;
	}
	req.url = url;
	req.method = (char *)"GET";
	req.callback = sse_collect;
	req.user_data = &g_collector;

	memset(&g_collector, 0, sizeof(g_collector));

	TEST_CHECK_INT_EQ(http_request(&req, &resp), 0);
	TEST_CHECK_INT_EQ(resp.status_code, 200);
	TEST_CHECK_STR_EQ(g_collector.buf, "firstsecond");

	http_response_free(&resp);
}

/* Abort check that fires immediately (before any data): models the user
 * pressing ESC while the request is still waiting for the response. */
static int abort_immediately(void *user_data)
{
	(void)user_data;
	return 1;
}

/* Abort check that fires only once some SSE data has been dispatched to
 * the collector: models ESC pressed after the first chunk arrived. The
 * libcurl progress callback runs periodically (also while the stream
 * stalls), so this fires during the slowstream stall. */
static int abort_after_first_event(void *user_data)
{
	(void)user_data;
	return g_collector.len > 0;
}

static void test_http_abort_nonstreaming(void)
{
	char url[128];
	struct http_request_t req = {0};
	struct http_response_t resp = {0};

	/* The slownonstream scenario stalls before answering: the abort
	 * must fire through the progress callback even though no data ever
	 * arrives (ESC must block non-streaming interactions too). */
	if (build_url(url, sizeof(url), "slownonstream/v1/chat/completions") != 0) {
		return;
	}
	req.url = url;
	req.method = (char *)"POST";
	req.body = (char *)"{\"model\":\"test-model\",\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}]}";
	req.abort_check = abort_immediately;
	req.abort_user_data = NULL;

	TEST_CHECK_INT_EQ(http_request(&req, &resp), 2);

	http_response_free(&resp);
}

static void test_http_abort_streaming_partial_body(void)
{
	char url[128];
	struct http_request_t req = {0};
	struct http_response_t resp = {0};

	/* The slowstream scenario sends one SSE event immediately, then
	 * stalls: the abort must fire during the stall (progress callback)
	 * while the first event was already dispatched to the callback. */
	if (build_url(url, sizeof(url), "slowstream/v1/chat/completions") != 0) {
		return;
	}
	req.url = url;
	req.method = (char *)"POST";
	req.body = (char *)"{\"model\":\"test-model\",\"messages\":[{\"role\":\"user\",\"content\":\"Hi\"}],\"stream\":true}";
	req.headers = (char **)((char *[]){ (char *)"Content-Type: application/json",
					   (char *)"Authorization: Bearer test-key-123" });
	req.headers_count = 2;
	req.callback = sse_collect;
	req.user_data = &g_collector;
	req.abort_check = abort_after_first_event;
	req.abort_user_data = NULL;

	memset(&g_collector, 0, sizeof(g_collector));

	TEST_CHECK_INT_EQ(http_request(&req, &resp), 2);
	/* The first streamed event was dispatched before the abort (the
	 * collector receives the raw SSE payload: the JSON chunk). */
	TEST_CHECK_NOT_NULL(strstr(g_collector.buf, "\"Hello\""));

	http_response_free(&resp);
}

static void test_http_response_free_null_safe(void)
{
	/* NULL and double free must not crash. */
	http_response_free(NULL);

	{
		struct http_response_t resp = {0};
		http_response_free(&resp);
		http_response_free(&resp);
	}
}

static void test_http_stream_sse(void)
{
	char url[128];
	struct http_request_t req = {0};
	struct http_response_t resp = {0};

	if (build_url(url, sizeof(url), "sse") != 0) {
		return;
	}
	req.url = url;
	req.method = (char *)"GET";
	req.body = NULL;
	req.headers = NULL;
	req.headers_count = 0;
	req.callback = sse_collect;
	req.user_data = &g_collector;

	memset(&g_collector, 0, sizeof(g_collector));

	TEST_CHECK_INT_EQ(http_request(&req, &resp), 0);
	TEST_CHECK_INT_EQ(resp.status_code, 200);
	/* The "data: " prefix must be stripped; one callback per event. */
	TEST_CHECK_STR_EQ(g_collector.buf, "firstsecond");

	http_response_free(&resp);
}

int main(void)
{
	TEST_CHECK_INT_EQ(http_client_init(), 0);

	TEST_RUN(test_http_request_post_success);
	TEST_RUN(test_http_request_nonexistent_scenario);
	TEST_RUN(test_http_request_unreachable);
	TEST_RUN(test_http_response_free_null_safe);
	TEST_RUN(test_http_stream_sse);
	TEST_RUN(test_http_stream_sse_crlf);
	TEST_RUN(test_http_stream_sse_no_space);
	TEST_RUN(test_http_abort_nonstreaming);
	TEST_RUN(test_http_abort_streaming_partial_body);

	http_client_cleanup();

	if (test__failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", test__failures);
		return 1;
	}
	printf("All http_client tests passed\n");
	return 0;
}
