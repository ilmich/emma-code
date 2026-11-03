#include "cjson/cJSON.h"
#include "openai_client.h"
#include "http_client.h"
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#define OPENAI_API_URL "/v1/chat/completions"

/**
 * @brief Constructs the JSON request body for the chat completion API.
 *
 * @param config The client configuration.
 * @param messages Array of messages.
 * @param message_count Number of messages.
 * @param model_name Model to use.
 * @param payload Buffer to write the JSON into.
 * @param payload_size Size of the buffer.
 * @return size_t The length of the written JSON string.
 */
static char* build_json_payload(
    const openai_config_t *config,
    const openai_message_t *messages,
    size_t message_count,
    int stream)
{
    // Create the root JSON object
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    if (config->model_name == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    // Add model name
    if (cJSON_AddStringToObject(root, "model", config->model_name) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    // Add sampling temperature
    if (cJSON_AddNumberToObject(root, "temperature", config->temperature) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    // Create and populate messages array
    cJSON *messages_array = cJSON_CreateArray();
    if (messages_array == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    for (size_t i = 0; i < message_count; ++i) {
        if (messages[i].role == NULL || messages[i].content == NULL) {
            cJSON_Delete(messages_array);
            cJSON_Delete(root);
            return NULL;
        }
        cJSON *message_item = cJSON_CreateObject();
        if (message_item == NULL) {
            cJSON_Delete(messages_array);
            cJSON_Delete(root);
            return NULL;
        }
        if (cJSON_AddStringToObject(message_item, "role", messages[i].role) == NULL ||
            cJSON_AddStringToObject(message_item, "content", messages[i].content) == NULL) {
            cJSON_Delete(message_item);
            cJSON_Delete(messages_array);
            cJSON_Delete(root);
            return NULL;
        }
        if (cJSON_AddItemToArray(messages_array, message_item) == 0) {
            cJSON_Delete(message_item);
            cJSON_Delete(messages_array);
            cJSON_Delete(root);
            return NULL;
        }
    }

    if (cJSON_AddItemToObject(root, "messages", messages_array) == 0) {
        cJSON_Delete(messages_array); // Not attached: free it separately
        cJSON_Delete(root);
        return NULL;
    }

    // Request a streamed (SSE) completion when configured.
    if (stream && cJSON_AddBoolToObject(root, "stream", 1) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    // Serialize the JSON object to a string
    char *json_string = cJSON_Print(root);
    if (json_string == NULL) {
        fprintf(stderr, "Error: Failed to serialize JSON payload.\n");
    }
    
    // Clean up the cJSON structure
    cJSON_Delete(root);
    
    return json_string;
}
/**
 * @brief Parses the response body to extract the assistant's content using cJSON.
 *
 * @param body The raw JSON response body.
 * @param response Pointer to the response structure to populate.
 * @return 0 on success, 1 on failure (parsing error).
 */
static int parse_response(const char *body, openai_response_t *response) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        fprintf(stderr, "Error: Failed to parse JSON response.\n");
        return 1;
    }

    // Navigate to choices array
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        fprintf(stderr, "Error: 'choices' array missing or empty in response.\n");
        cJSON_Delete(root);
        return 1;
    }

    // Navigate to the first choice
    cJSON *choice = cJSON_GetArrayItem(choices, 0);
    
    // Navigate to message object
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    if (!cJSON_IsObject(message)) {
        fprintf(stderr, "Error: 'message' object missing in choice.\n");
        cJSON_Delete(root);
        return 1;
    }

    // Navigate to content field
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (!cJSON_IsString(content)) {
        fprintf(stderr, "Error: 'content' string missing in message.\n");
        cJSON_Delete(root);
        return 1;
    }

    // Extract and copy the content string
    const char *content_str = cJSON_GetStringValue(content);
    size_t length = strlen(content_str);

    // Allocate memory for the content
    char *content_copy = (char *)malloc(length + 1);
    if (!content_copy) {
        perror("Failed to allocate memory for response content");
        cJSON_Delete(root);
        return 1;
    }

    memcpy(content_copy, content_str, length + 1);
    response->content = content_copy;

    // Clean up
    cJSON_Delete(root);
    return 0;
}

/**
 * @brief Internal context handed to the SSE event collector via user_data.
 */
struct stream_ctx {
    char *content;                       /**< accumulated full text (owned) */
    size_t len;                          /**< current length of content */
    size_t cap;                          /**< allocated capacity of content */
    openai_delta_callback_t on_delta;    /**< per-delta callback (may be NULL) */
    void *user_data;                     /**< opaque pointer for on_delta */
    int got_event;                       /**< set once at least one SSE event arrived */
    int oom;                             /**< set on allocation failure */
};

/**
 * @brief Appends a delta to the accumulated stream content.
 *
 * @return 0 on success, -1 on allocation failure (ctx->oom is set).
 */
static int stream_accumulate(struct stream_ctx *ctx, const char *text,
                             size_t text_len)
{
    if (ctx->len + text_len + 1 > ctx->cap) {
        size_t new_cap = ctx->cap ? ctx->cap : 64;
        char *tmp;

        while (new_cap < ctx->len + text_len + 1) {
            new_cap *= 2;
        }
        tmp = realloc(ctx->content, new_cap);
        if (tmp == NULL) {
            fprintf(stderr, "Error: out of memory while accumulating "
                    "streamed response\n");
            ctx->oom = 1;
            return -1;
        }
        ctx->content = tmp;
        ctx->cap = new_cap;
    }
    memcpy(ctx->content + ctx->len, text, text_len);
    ctx->len += text_len;
    ctx->content[ctx->len] = '\0';
    return 0;
}

/**
 * @brief http_stream_callback_t collector for OpenAI streaming events.
 *
 * Receives the raw payload of a single SSE event (the "data:" prefix is
 * already stripped by the HTTP layer). Recognizes "[DONE]", parses the
 * JSON chunk and forwards choices[0].delta.content to the user callback
 * while accumulating the full text. Malformed or metadata-only chunks are
 * ignored with a warning; allocation failures are recorded in the context
 * and handled by the owning caller.
 */
static void stream_event(const char *buffer, size_t size, void *user_data)
{
    struct stream_ctx *ctx = (struct stream_ctx *)user_data;
    char *payload;
    cJSON *root;
    cJSON *choices;
    cJSON *choice;
    cJSON *delta;
    cJSON *content;
    const char *text;

    if (ctx == NULL) {
        return;
    }
    ctx->got_event = 1;

    if (size == 6 && memcmp(buffer, "[DONE]", 6) == 0) {
        return;
    }

    // The payload is not NUL-terminated: copy it before cJSON_Parse().
    payload = (char *)malloc(size + 1);
    if (payload == NULL) {
        fprintf(stderr, "Error: out of memory while parsing stream chunk\n");
        ctx->oom = 1;
        return;
    }
    memcpy(payload, buffer, size);
    payload[size] = '\0';

    root = cJSON_Parse(payload);
    free(payload);
    if (root == NULL) {
        fprintf(stderr, "Warning: ignoring malformed stream chunk\n");
        return;
    }

    choices = cJSON_GetObjectItem(root, "choices");
    choice = (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0)
                 ? cJSON_GetArrayItem(choices, 0)
                 : NULL;
    delta = (choice != NULL) ? cJSON_GetObjectItem(choice, "delta") : NULL;
    content = (cJSON_IsObject(delta))
                  ? cJSON_GetObjectItem(delta, "content")
                  : NULL;

    // Metadata-only chunks carry no delta content: ignore silently.
    if (content != NULL && cJSON_IsString(content) &&
        content->valuestring != NULL &&
        content->valuestring[0] != '\0') {
        text = content->valuestring;
        if (stream_accumulate(ctx, text, strlen(text)) == 0 &&
            ctx->on_delta != NULL) {
            ctx->on_delta(text, ctx->user_data);
        }
    }

    cJSON_Delete(root);
}

/**
 * @brief Sends a chat completion request and returns the raw HTTP response.
 *
 * Builds URL, headers and JSON payload (with "stream": true when @p stream
 * is nonzero), executes the request and validates the status code. On any
 * failure the response is freed and 1 is returned; on success *resp is
 * populated and owned by the caller (free with http_response_free()).
 */
static int send_request(
    const openai_config_t *config,
    const openai_message_t *messages,
    size_t message_count,
    int stream,
    http_stream_callback_t stream_callback,
    void *stream_user_data,
    struct http_response_t *resp)
{
    // For simplicity, we assume config->base_url is just the root
    // (e.g., "https://api.openai.com") and we append the endpoint.
    char url_buffer[512];
    char auth_header[512];
    char *headers[2];
    size_t headers_count = 0;
    char *payload = NULL;
    struct http_request_t req;
    int status = 1;
    int n;

    // The API key is optional: only add the Authorization header when one
    // is configured.
    headers[headers_count++] = "Content-Type: application/json";
    if (config->api_key != NULL) {
        n = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", config->api_key);
        if (n < 0 || (size_t)n >= sizeof(auth_header)) {
            fprintf(stderr, "Error: auth_header buffer truncated\n");
            return 1;
        }
        headers[headers_count++] = auth_header;
    }
    n = snprintf(url_buffer, sizeof(url_buffer), "%s%s", config->base_url, OPENAI_API_URL);
    if (n < 0 || (size_t)n >= sizeof(url_buffer)) {
        fprintf(stderr, "Error: url_buffer truncated\n");
        return 1;
    }

    // Build the JSON payload
    payload = build_json_payload(config, messages, message_count, stream);
    if (payload == NULL) {
        fprintf(stderr, "Error: Failed to build JSON payload.\n");
        return 1;
    }

    // Prepare and execute the HTTP Request
    req.url = url_buffer;
    req.method = "POST";
    req.body = payload;
    req.headers = headers;
    req.headers_count = headers_count;
    req.callback = stream_callback;
    req.user_data = stream_user_data;

    memset(resp, 0, sizeof(*resp));
    if (http_request(&req, resp) != 0) {
        fprintf(stderr, "HTTP Request failed.\n");
        goto cleanup;
    }

    // Handle HTTP-level errors
    if (resp->status_code != 200) {
        fprintf(stderr, "API Error: Received status code %d. Body: %s\n",
                resp->status_code, resp->body ? resp->body : "No body");
        goto cleanup;
    }

    status = 0;

cleanup:
    free(payload);
    if (status != 0) {
        http_response_free(resp);
    }
    return status;
}

int openai_chat_completion(
    const openai_config_t *config,
    const openai_message_t *messages,
    size_t message_count,
    openai_response_t *response) 
{
    struct http_response_t resp;
    int parse_status;

    if (send_request(config, messages, message_count, 0, NULL, NULL,
                     &resp) != 0) {
        return 1;
    }

    // Parse the response, then clean up.
    parse_status = parse_response(resp.body, response);
    http_response_free(&resp);
    return parse_status;
}

int openai_chat_completion_stream(
    const openai_config_t *config,
    const openai_message_t *messages,
    size_t message_count,
    openai_delta_callback_t on_delta,
    void *user_data,
    openai_response_t *response)
{
    struct stream_ctx ctx = {0};
    struct http_response_t resp;
    int status = 1;

    ctx.on_delta = on_delta;
    ctx.user_data = user_data;

    if (send_request(config, messages, message_count, 1, stream_event,
                     &ctx, &resp) != 0) {
        goto cleanup;
    }

    if (ctx.got_event) {
        // Server honored "stream": true. The accumulated buffer holds the
        // full text; an event-only stream without content yields "".
        if (!ctx.oom) {
            if (ctx.content != NULL) {
                response->content = ctx.content;
                ctx.content = NULL; // ownership transferred
            } else {
                response->content = (char *)malloc(1);
                if (response->content == NULL) {
                    fprintf(stderr, "Error: out of memory for response content\n");
                } else {
                    response->content[0] = '\0';
                }
            }
        }
        status = (ctx.oom || response->content == NULL) ? 1 : 0;
    } else {
        // The server ignored "stream": true and answered with a plain JSON
        // body: fall back to the non-streaming parser.
        status = parse_response(resp.body, response);
    }

cleanup:
    http_response_free(&resp);
    free(ctx.content);
    return status;
}

void openai_response_free(openai_response_t *response) {
    if (response && response->content) {
        free(response->content);
        response->content = NULL;
    }
}
