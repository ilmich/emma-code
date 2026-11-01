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
    size_t last_pos;
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
                    mem->callback(line_ptr + off, line_len - off);
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

int http_request(const struct http_request_t *req, struct http_response_t *resp) {
    CURL *curl;
    CURLcode res;
    struct mem_struct chunk;
    long response_code;

    curl = curl_easy_init();
    if (!curl) {
        return 1;
    }

    chunk.data = malloc(1);    
    if (!chunk.data) {
        curl_easy_cleanup(curl);
        return 1;
    }
    // Always NUL-terminate so the body is a valid C string even if the
    // response has an empty body.
    chunk.data[0] = '\0';
    chunk.size = 0;
    chunk.headers = NULL;
    chunk.headers_count = 0;
    chunk.callback = req->callback;
    chunk.last_pos = 0;
    chunk.streaming = 0;

    curl_easy_setopt(curl, CURLOPT_URL, req->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EmmaCode HTTP Client");
    /* Never hang forever on a stalled connection; a low total timeout
     * would kill legitimate long-lived LLM responses, so guard with a
     * connect timeout and a low-speed stall detector instead. */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    /* Avoid SIGPIPE/longjmp-based DNS timeouts: the TUI installs its own
     * signal handlers and must not receive SIGPIPE from libcurl. */
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (req->method && strcmp(req->method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
    }

    struct curl_slist *header_list = NULL;
    if (req->headers && req->headers_count > 0) {
        for (size_t i = 0; i < req->headers_count; i++) {
            struct curl_slist *tmp = curl_slist_append(header_list, req->headers[i]);
            if (!tmp) {
                fprintf(stderr, "curl_slist_append() failed\n");
                curl_slist_free_all(header_list);
                free(chunk.data);
                curl_easy_cleanup(curl);
                return 1;
            }
            header_list = tmp;
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }

    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)&chunk);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(chunk.data);
        curl_easy_cleanup(curl);
        if (header_list) {
            curl_slist_free_all(header_list);
        }
        if (chunk.headers) {
            for (size_t i = 0; i < chunk.headers_count; i++) {
                free(chunk.headers[i]);
            }
        }
	free(chunk.headers);
        return 1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
    resp->status_code = (int)response_code;

    resp->body = chunk.data;
    resp->body_len = chunk.size;
    resp->headers = chunk.headers;
    resp->headers_count = chunk.headers_count;

    curl_easy_cleanup(curl);
    if (header_list) {
        curl_slist_free_all(header_list);
    }
    return 0;
}

void http_response_free(struct http_response_t* response) {
    if (response == NULL) {
        return;
    }

    // Free the body string
    if (response->body != NULL) {
        free(response->body);
        response->body = NULL;
    }

    // Free each header string
    if (response->headers != NULL) {
        for (size_t i = 0; i < response->headers_count; i++) {
            if (response->headers[i] != NULL) {
                free(response->headers[i]);
                response->headers[i] = NULL;
            }
        }
	// Free the array of header pointers
	free(response->headers);
	response->headers = NULL;
	response->headers_count = 0;
    }

}