#include <stdio.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "http_client.h"

// Callback to write response data into a string
struct mem_struct {
    char *data;
    size_t size;
    int streaming;
    char **headers;
    size_t headers_count;
    http_stream_callback_t callback;
    void *user_data;
    size_t last_pos;
    int (*abort_fn)(void *abort_user_data); /* optional abort check */
    void *abort_data;
    int aborted;                            /* set when abort fired */
};

// Case-insensitive check whether a header line announces an SSE stream.
static int header_is_sse(const char *header) {
    const char *value;
    const char *needle = "text/event-stream";
    size_t needle_len = strlen(needle);

    if (strncasecmp(header, "content-type:", 13) != 0) {
        return 0;
    }
    value = header + 13;
    for (const char *p = value; *p != '\0'; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Finds the end of the SSE event starting at `p` (i.e. the first blank
 * line). Line terminators may be LF, CRLF or CR, so "\n\n", "\r\n\r\n"
 * and mixed "\n\r\n" all terminate an event.
 *
 * Returns a pointer just past the blank line, or NULL when the buffer
 * ends inside a (possibly still incomplete) line terminator: a trailing
 * CR must not be treated as a boundary because the matching LF may only
 * arrive with the next chunk.
 */
static char *find_event_end(char *p, char *end) {
    while (p < end) {
        if (*p == '\n') {
            return p + 1; /* empty line: event boundary */
        }
        if (*p == '\r') {
            if (p + 1 >= end) {
                return NULL; /* CRLF possibly split across chunks */
            }
            if (p[1] == '\n') {
                return p + 2;
            }
            return p + 1; /* bare CR: empty line */
        }
        while (p < end && *p != '\n' && *p != '\r') {
            p++;
        }
    }
    return NULL;
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct mem_struct *mem = (struct mem_struct *)userp;

    /* Abort requested: returning 0 makes libcurl fail the transfer with
     * CURLE_WRITE_ERROR; http_request() then reports it as an abort. */
    if (mem->abort_fn && mem->abort_fn(mem->abort_data)) {
        mem->aborted = 1;
        return 0;
    }

    char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "not enough memory\n");
        return 0;
    }

    mem->data = ptr;
    memcpy(ptr + mem->size, contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    if (mem->streaming && mem->callback) {
        while (1) {
            char *boundary = find_event_end(mem->data + mem->last_pos,
                                             mem->data + mem->size);
            if (!boundary) {
                break;
            }

            size_t msg_start = mem->last_pos;
            size_t msg_end = (size_t)(boundary - mem->data);

            char *line_ptr = mem->data + msg_start;
            while (line_ptr < mem->data + msg_end) {
                char *line_end = line_ptr;
                while (line_end < mem->data + msg_end && *line_end != '\n') {
                    line_end++;
                }

                size_t line_len = line_end - line_ptr;
                /* Strip a trailing CR from CRLF line endings. */
                if (line_len > 0 && line_ptr[line_len - 1] == '\r') {
                    line_len--;
                }
                /* "data:" with an optional single space, per the SSE spec. */
                if (line_len >= 5 && strncmp(line_ptr, "data:", 5) == 0) {
                    size_t off = 5;

                    if (off < line_len && line_ptr[off] == ' ') {
                        off++;
                    }
                    mem->callback(line_ptr + off, line_len - off,
                                  mem->user_data);
                }

                line_ptr = (line_end < mem->data + msg_end)
                               ? line_end + 1
                               : line_end;
            }

            mem->last_pos = msg_end;
        }
    }

    return realsize;
}

static size_t header_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct mem_struct *hs = (struct mem_struct *)userp;

    char *header = malloc(realsize + 1);
    if (!header) {
        return 0;
    }
    memcpy(header, contents, realsize);
    header[realsize] = '\0';

    // Remove trailing \r\n
    for (size_t i = 0; i < realsize; i++) {
        if (header[i] == '\r') {
            header[i] = '\0';
            break;
        }
    }

    if (header_is_sse(header)) {
        hs->streaming = 1;
    }

    char **new_headers = realloc(hs->headers, sizeof(char *) * (hs->headers_count + 1));
    if (!new_headers) {
        free(header);
        return 0;
    }
    hs->headers = new_headers;
    hs->headers[hs->headers_count] = header;
    hs->headers_count++;

    return realsize;
}

int http_client_init(void) {
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_global_init() failed: %s\n", curl_easy_strerror(res));
        return 1;
    }
    return 0;
}

void http_client_cleanup(void) {
    curl_global_cleanup();
}

/** Frees an array of `count` header strings plus the array itself. */
static void http_headers_free(char **headers, size_t count) {
    if (headers == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(headers[i]);
    }
    free(headers);
}

/* Libcurl progress callback: invoked periodically even when no data is
 * flowing (e.g. while waiting for the server response), so an abort is
 * honored also for non-streaming interactions. Returning nonzero aborts
 * the transfer with CURLE_ABORTED_BY_CALLBACK. */
static int xferinfo_callback(void *clientp,
                             curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    struct mem_struct *mem = (struct mem_struct *)clientp;

    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    if (mem->abort_fn && mem->abort_fn(mem->abort_data)) {
        mem->aborted = 1;
        return 1;
    }
    return 0;
}

int http_request(const struct http_request_t *req, struct http_response_t *resp) {
    CURL *curl = NULL;
    CURLcode res;
    struct mem_struct chunk = {0};
    struct curl_slist *header_list = NULL;
    long response_code;
    int status = 1;

    curl = curl_easy_init();
    if (!curl) {
        return 1;
    }

    chunk.data = malloc(1);
    if (!chunk.data) {
        goto cleanup;
    }
    // Always NUL-terminate so the body is a valid C string even if the
    // response has an empty body.
    chunk.data[0] = '\0';
    chunk.size = 0;
    chunk.callback = req->callback;
    chunk.user_data = req->user_data;
    chunk.last_pos = 0;
    chunk.streaming = 0;
    chunk.abort_fn = req->abort_check;
    chunk.abort_data = req->abort_user_data;
    chunk.aborted = 0;

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EmmaCode HTTP Client");
    /* Never hang forever on a stalled connection; a low total timeout
     * would kill legitimate long-lived LLM responses, so guard with a
     * connect timeout and a low-speed stall detector instead. The stall
     * timeout is configurable (HTTP_TIMEOUT_SECS by default). */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,
                     req->timeout_secs > 0 ? req->timeout_secs
                                           : HTTP_TIMEOUT_SECS);
    /* Avoid SIGPIPE/longjmp-based DNS timeouts: the TUI installs its own
     * signal handlers and must not receive SIGPIPE from libcurl. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (req->method && strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
    }

    if (req->headers && req->headers_count > 0) {
        for (size_t i = 0; i < req->headers_count; i++) {
            struct curl_slist *tmp = curl_slist_append(header_list, req->headers[i]);
            if (!tmp) {
                fprintf(stderr, "curl_slist_append() failed\n");
                goto cleanup;
            }
            header_list = tmp;
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)&chunk);

    if (req->abort_check) {
        /* Enable the progress callback so the abort check runs even
         * while no data is flowing (waiting for the response). */
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)&chunk);
    }

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        if (chunk.aborted ||
            (req->abort_check && req->abort_check(req->abort_user_data))) {
            /* Aborted on purpose: hand the partial body back to the
             * caller (it decides whether to use it) and report 2. */
            resp->body = chunk.data;
            resp->body_len = chunk.size;
            resp->headers = chunk.headers;
            resp->headers_count = chunk.headers_count;
            chunk.data = NULL;      /* ownership transferred to resp */
            chunk.headers = NULL;
            status = 2;
            goto cleanup;
        }
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        goto cleanup;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    resp->status_code = (int)response_code;

    resp->body = chunk.data;
    resp->body_len = chunk.size;
    resp->headers = chunk.headers;
    resp->headers_count = chunk.headers_count;
    chunk.data = NULL;      /* ownership transferred to resp */
    chunk.headers = NULL;
    status = 0;

cleanup:
    if (curl) {
        curl_easy_cleanup(curl);
    }
    if (header_list) {
        curl_slist_free_all(header_list);
    }
    free(chunk.data);
    http_headers_free(chunk.headers, chunk.headers_count);
    return status;
}

void http_response_free(struct http_response_t* response) {
    if (response == NULL) {
        return;
    }

    // Free the body string
    free(response->body);
    response->body = NULL;

    // Free each header string and the array of header pointers
    http_headers_free(response->headers, response->headers_count);
    response->headers = NULL;
    response->headers_count = 0;
}