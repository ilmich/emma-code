/**
 * @file http_client.h
 * @brief Defines the interface for the HTTP client module.
 *
 * This module provides a C interface to perform HTTP requests using libcurl.
 * It supports both standard request/response cycles and streaming responses.
 */

/**
 * @brief Example Usage Demonstrations
 *
 * The following are conceptual examples of how to integrate the HTTP client.
 *
 * 1. Non-Streaming (Standard Request/Response)
 * -------------------------------------------------
 *
 *
 * // Setup request structure
 * struct http_request_t req = {
 *     .url = "https://api.example.com/data",
 *     .method = "GET",
 *     .body = NULL,
 *     .headers = NULL,
 *     .headers_count = 0,
 *     .callback = NULL,
 *     .user_data = NULL
 * };
 *
 * // Setup response structure
 * struct http_response_t resp = {0};
 *
 * // Execute the request
 * if (http_request(&req, &resp) == 0) {
 *     // Request successful. Process response body: resp.body
 *     http_response_free(&resp);
 * }
 *
 * // 2. Streaming Response
 * -------------------------------------------------
 *
 * // Define a callback that processes chunks as they arrive
 * void chunk_processor(const char* buffer, size_t size) {
 *     // Process the chunk immediately (e.g., print to console, write to file)
 *     printf("Received event of size %zu: %.*s\n", size, (int)size, buffer);
 * }
 *
 * // Setup request structure for streaming
 * struct http_request_t stream_req = {
 *     .url = "https://api.example.com/stream",
 *     .method = "GET",
 *     .body = NULL,
 *     .headers = NULL,
 *     .headers_count = 0,
 *     .callback = chunk_processor, // Use the chunk processor
 *     .user_data = NULL
 * };
 *
 * // Setup response structure
 * struct http_response_t stream_resp = {0};
 *
 * // Execute the request
 * if (http_request(&stream_req, &stream_resp) == 0) {
 *     // Streaming request completed. Final status code is in stream_resp.status_code
 *     http_response_free(&stream_resp);
 * }
*/

#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback function signature for streaming response data.
 *
 * This function is called repeatedly as chunks of the response body are received.
 * The implementation ensures that the 'data:' prefix of the Server-Sent Events (SSE)
 * event is stripped, and the callback receives only the raw payload.
 *
 * @param buffer Pointer to the received data chunk (NOT NUL-terminated).
 * @param size The size of the data chunk in bytes.
 * @param user_data The user_data pointer from the http_request_t structure.
 */
typedef void (*http_stream_callback_t)(const char* buffer, size_t size,
                                       void* user_data);

/**
 * @brief Structure defining the parameters for an HTTP request.
 */
struct http_request_t {
    char* url;               ///< The target URL.
    char* method;           ///< The HTTP method (e.g., "GET", "POST").
    char* body;             ///< The request body payload (used for POST/PUT).
    char** headers;        ///< Array of custom HTTP headers.
    size_t headers_count;        ///< Number of headers in the array.
    http_stream_callback_t callback; ///< Callback to handle incoming streamed data.
    void* user_data;              ///< Pointer to user-defined data, passed to the callback.
};

/**
 * @brief Structure holding the results of an HTTP response.
 *
 * NOTE: The caller is responsible for freeing the memory allocated for 'body' and 'headers'.
 *       Use http_response_free() to perform this cleanup.
 */
struct http_response_t {
    int status_code;             ///< The HTTP status code (e.g., 200, 404).
    char* body;                  ///< Dynamically allocated string containing the response body.
    size_t body_len;             ///< Length of the response body.
    char** headers;             ///< Array of dynamically allocated response headers.
    size_t headers_count;       ///< Number of response headers.
};

/**
 * @brief Initializes the HTTP client module.
 *
 * Must be called once before any other http_* function.
 * Internally calls curl_global_init(CURL_GLOBAL_DEFAULT).
 *
 * @return 0 on success, non-zero on failure.
 */
int http_client_init(void);

/**
 * @brief Cleans up the HTTP client module.
 *
 * Must be called once at program exit, after all http_* calls are done.
 * Internally calls curl_global_cleanup().
 */
void http_client_cleanup(void);

/**
 * @brief Executes an HTTP request.
 *
 * @param req Pointer to the request structure.
 * @param resp Pointer to the response structure to be populated.
 * @return 0 on success, 1 on failure (e.g., network error, memory allocation failure).
 */
int http_request(const struct http_request_t *req, struct http_response_t *resp);

/**
 * @brief Frees all dynamically allocated memory within an http_response_t structure.
 *
 * This function frees the response body and all headers (both the strings and the array of pointers).
 * It does not free the structure itself.
 *
 * @param response Pointer to the http_response_t structure to be cleaned up.
 */
void http_response_free(struct http_response_t* response);

#ifdef __cplusplus
}
#endif

#endif // HTTP_CLIENT_H
