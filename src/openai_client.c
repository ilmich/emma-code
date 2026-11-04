#include "cjson/cJSON.h"
#include "openai_client.h"
#include "http_client.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#define OPENAI_API_URL "/v1/chat/completions"

void openai_tool_calls_free(openai_tool_call_t *calls, size_t count)
{
    if (calls == NULL)
        return;
    for (size_t i = 0; i < count; ++i) {
        free(calls[i].id);
        free(calls[i].name);
        free(calls[i].arguments);
    }
    free(calls);
}

void openai_message_free(openai_message_t *msg)
{
    if (msg == NULL)
        return;
    free(msg->role);
    free(msg->content);
    free(msg->tool_call_id);
    openai_tool_calls_free(msg->tool_calls, msg->tool_call_count);
    msg->role = NULL;
    msg->content = NULL;
    msg->tool_call_id = NULL;
    msg->tool_calls = NULL;
    msg->tool_call_count = 0;
}

/**
 * @brief Parses a message-level "tool_calls" JSON array into a malloc'ed
 *        openai_tool_call_t array.
 *
 * @param arr cJSON array (must be valid).
 * @param out Receives the malloc'ed array (owned by the caller).
 * @param count Receives the number of parsed entries.
 * @return 0 on success, -1 on out of memory (out is left NULL).
 */
static int parse_tool_calls(const cJSON *arr, openai_tool_call_t **out,
                            size_t *count)
{
    size_t n = 0;
    openai_tool_call_t *calls = NULL;

    *out = NULL;
    *count = 0;
    n = (size_t)cJSON_GetArraySize(arr);
    if (n == 0)
        return 0;
    calls = (openai_tool_call_t *)calloc(n, sizeof(*calls));
    if (calls == NULL)
        return -1;
    for (size_t i = 0; i < n; ++i) {
        const cJSON *item = cJSON_GetArrayItem(arr, (int)i);
        const cJSON *fn = cJSON_GetObjectItem(item, "function");
        const cJSON *id = cJSON_GetObjectItem(item, "id");
        const cJSON *name = cJSON_GetObjectItem(fn, "name");
        const cJSON *args = cJSON_GetObjectItem(fn, "arguments");

        if (cJSON_IsString(id)) {
            calls[i].id = util_strdup(id->valuestring);
            if (calls[i].id == NULL)
                goto oom;
        }
        if (cJSON_IsString(name)) {
            calls[i].name = util_strdup(name->valuestring);
            if (calls[i].name == NULL)
                goto oom;
        }
        if (cJSON_IsString(args)) {
            calls[i].arguments = util_strdup(args->valuestring);
            if (calls[i].arguments == NULL)
                goto oom;
        }
    }
    *out = calls;
    *count = n;
    return 0;

oom:
    openai_tool_calls_free(calls, n);
    return -1;
}

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

    // Announce the available tools when configured (pre-serialized JSON
    // array produced by the tool registry).
    if (config->tools_json != NULL && config->tools_json[0] != '\0') {
        cJSON *tools = cJSON_Parse(config->tools_json);
        if (tools == NULL || !cJSON_IsArray(tools) ||
            cJSON_AddItemToObject(root, "tools", tools) == 0) {
            if (tools != NULL && cJSON_GetObjectItem(root, "tools") == NULL)
                cJSON_Delete(tools); // not attached: free it separately
            cJSON_Delete(root);
            return NULL;
        }
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
        /* content is optional only for assistant messages that carry
         * tool_calls; every other message requires it. */
        int has_tool_calls = messages[i].tool_calls != NULL &&
                             messages[i].tool_call_count > 0;
        if (messages[i].role == NULL ||
            (messages[i].content == NULL && !has_tool_calls)) {
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
        if (strcmp(messages[i].role, "tool") == 0 &&
            messages[i].tool_call_id != NULL) {
            /* Tool result message: role, content and the id of the
             * answered call. */
            if (cJSON_AddStringToObject(message_item, "role",
                                        messages[i].role) == NULL ||
                cJSON_AddStringToObject(message_item, "content",
                                        messages[i].content) == NULL ||
                cJSON_AddStringToObject(message_item, "tool_call_id",
                                        messages[i].tool_call_id) == NULL) {
                cJSON_Delete(message_item);
                cJSON_Delete(messages_array);
                cJSON_Delete(root);
                return NULL;
            }
        } else if (messages[i].tool_calls != NULL &&
                   messages[i].tool_call_count > 0) {
            /* Assistant message requesting tool executions: content is
             * optional, the tool_calls array is mandatory. */
            cJSON *calls_array = cJSON_CreateArray();
            if (calls_array == NULL) {
                cJSON_Delete(message_item);
                cJSON_Delete(messages_array);
                cJSON_Delete(root);
                return NULL;
            }
            if (cJSON_AddStringToObject(message_item, "role",
                                        messages[i].role) == NULL ||
                (messages[i].content != NULL &&
                 cJSON_AddStringToObject(message_item, "content",
                                         messages[i].content) == NULL)) {
                cJSON_Delete(calls_array);
                cJSON_Delete(message_item);
                cJSON_Delete(messages_array);
                cJSON_Delete(root);
                return NULL;
            }
            for (size_t j = 0; j < messages[i].tool_call_count; ++j) {
                const openai_tool_call_t *tc = &messages[i].tool_calls[j];
                cJSON *call = cJSON_CreateObject();
                cJSON *fn = NULL;

                if (call == NULL ||
                    (fn = cJSON_CreateObject()) == NULL ||
                    cJSON_AddStringToObject(call, "id",
                                            tc->id != NULL ? tc->id : "") == NULL ||
                    cJSON_AddStringToObject(call, "type", "function") == NULL ||
                    cJSON_AddItemToObject(call, "function", fn) == 0 ||
                    cJSON_AddStringToObject(fn, "name",
                                            tc->name != NULL ? tc->name : "") == NULL ||
                    cJSON_AddStringToObject(fn, "arguments",
                                            tc->arguments != NULL
                                                ? tc->arguments : "") == NULL) {
                    cJSON_Delete(call); // tolerates NULL
                    cJSON_Delete(calls_array);
                    cJSON_Delete(message_item);
                    cJSON_Delete(messages_array);
                    cJSON_Delete(root);
                    return NULL;
                }
                if (cJSON_AddItemToArray(calls_array, call) == 0) {
                    cJSON_Delete(call);
                    cJSON_Delete(calls_array);
                    cJSON_Delete(message_item);
                    cJSON_Delete(messages_array);
                    cJSON_Delete(root);
                    return NULL;
                }
            }
            if (cJSON_AddItemToObject(message_item, "tool_calls",
                                      calls_array) == 0) {
                cJSON_Delete(calls_array); // not attached
                cJSON_Delete(message_item);
                cJSON_Delete(messages_array);
                cJSON_Delete(root);
                return NULL;
            }
        } else if (cJSON_AddStringToObject(message_item, "role",
                                           messages[i].role) == NULL ||
                   cJSON_AddStringToObject(message_item, "content",
                                           messages[i].content) == NULL) {
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

    // Content is optional when the assistant requests tool executions.
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (cJSON_IsString(content) && content->valuestring != NULL) {
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
    }

    // Reasoning text (when the model provides it): prefer the
    // "reasoning_content" spelling, fall back to "reasoning".
    cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning_content");
    if (!cJSON_IsString(reasoning))
        reasoning = cJSON_GetObjectItem(message, "reasoning");
    if (cJSON_IsString(reasoning) && reasoning->valuestring != NULL &&
        reasoning->valuestring[0] != '\0') {
        char *reasoning_copy = (char *)malloc(strlen(reasoning->valuestring) + 1);
        if (!reasoning_copy) {
            perror("Failed to allocate memory for response reasoning");
            cJSON_Delete(root);
            return 1;
        }

        strcpy(reasoning_copy, reasoning->valuestring);
        response->reasoning = reasoning_copy;
    }

    // Extract the requested tool calls, if any.
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
        if (parse_tool_calls(tool_calls, &response->tool_calls,
                             &response->tool_call_count) != 0) {
            fprintf(stderr, "Error: out of memory parsing tool calls.\n");
            cJSON_Delete(root);
            return 1;
        }
    }

    // A message with neither content nor tool calls is unusable.
    if (response->content == NULL && response->tool_call_count == 0) {
        fprintf(stderr, "Error: 'content' string missing in message.\n");
        cJSON_Delete(root);
        return 1;
    }

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
    char *reasoning;                     /**< accumulated reasoning (owned) */
    size_t reasoning_len;                /**< current length of reasoning */
    size_t reasoning_cap;                /**< allocated capacity of reasoning */
    openai_tool_call_t *tool_calls;      /**< accumulated tool calls (owned) */
    size_t tool_call_count;              /**< number of accumulated calls */
    size_t tool_call_cap;                /**< allocated capacity of tool_calls */
    openai_delta_callback_t on_delta;    /**< per-delta callback (may be NULL) */
    void *user_data;                     /**< opaque pointer for on_delta */
    int got_event;                       /**< set once at least one SSE event arrived */
    int oom;                             /**< set on allocation failure */
};

/**
 * @brief Returns the accumulator slot for a streamed tool call index,
 *        growing the array as needed (indices arrive in order starting
 *        from 0, but any gap is filled with empty entries).
 *
 * @return Pointer to the slot, or NULL on allocation failure (ctx->oom).
 */
static openai_tool_call_t *stream_tool_slot(struct stream_ctx *ctx,
                                            size_t index)
{
    if (index >= ctx->tool_call_cap) {
        size_t new_cap = ctx->tool_call_cap ? ctx->tool_call_cap : 4;
        openai_tool_call_t *grown;

        while (new_cap <= index)
            new_cap *= 2;
        grown = (openai_tool_call_t *)realloc(
            ctx->tool_calls, new_cap * sizeof(*grown));
        if (grown == NULL) {
            fprintf(stderr, "Error: out of memory while accumulating "
                    "streamed tool calls\n");
            ctx->oom = 1;
            return NULL;
        }
        /* New slots start empty; existing slots above the count (gaps)
         * must be zeroed too. */
        for (size_t i = ctx->tool_call_cap; i < new_cap; ++i) {
            grown[i].id = NULL;
            grown[i].name = NULL;
            grown[i].arguments = NULL;
        }
        ctx->tool_calls = grown;
        ctx->tool_call_cap = new_cap;
    }
    if (index >= ctx->tool_call_count)
        ctx->tool_call_count = index + 1;
    return &ctx->tool_calls[index];
}

/**
 * @brief Appends a string to a slot field (id/name/arguments fragments
 *        are concatenated into the same allocation).
 *
 * @return 0 on success, -1 on allocation failure.
 */
static int stream_tool_append(char **field, const char *text)
{
    size_t old_len = *field != NULL ? strlen(*field) : 0;
    size_t text_len = strlen(text);
    char *grown = (char *)realloc(*field, old_len + text_len + 1);

    if (grown == NULL)
        return -1;
    memcpy(grown + old_len, text, text_len + 1);
    *field = grown;
    return 0;
}

/**
 * @brief Accumulates one delta.tool_calls fragment into the context.
 *
 * @return 0 on success, -1 on allocation failure (ctx->oom is set).
 */
static int stream_accumulate_tool_call(struct stream_ctx *ctx,
                                       const cJSON *fragment)
{
    const cJSON *index = cJSON_GetObjectItem(fragment, "index");
    const cJSON *id = cJSON_GetObjectItem(fragment, "id");
    const cJSON *fn = cJSON_GetObjectItem(fragment, "function");
    const cJSON *name = cJSON_GetObjectItem(fn, "name");
    const cJSON *args = cJSON_GetObjectItem(fn, "arguments");
    openai_tool_call_t *slot;

    if (!cJSON_IsNumber(index) || index->valuedouble < 0)
        return 0; // malformed fragment: ignore
    slot = stream_tool_slot(ctx, (size_t)index->valuedouble);
    if (slot == NULL)
        return -1;
    if (cJSON_IsString(id) && id->valuestring != NULL &&
        slot->id == NULL &&
        stream_tool_append(&slot->id, id->valuestring) != 0) {
        ctx->oom = 1;
        return -1;
    }
    if (cJSON_IsString(name) && name->valuestring != NULL &&
        slot->name == NULL &&
        stream_tool_append(&slot->name, name->valuestring) != 0) {
        ctx->oom = 1;
        return -1;
    }
    if (cJSON_IsString(args) && args->valuestring != NULL &&
        args->valuestring[0] != '\0' &&
        stream_tool_append(&slot->arguments, args->valuestring) != 0) {
        ctx->oom = 1;
        return -1;
    }
    return 0;
}

/**
 * @brief Appends text to a growable accumulator buffer (content or
 *        reasoning fragments are concatenated into the same allocation).
 *
 * @return 0 on success, -1 on allocation failure (ctx->oom is set).
 */
static int stream_buf_append(struct stream_ctx *ctx, char **buf,
                             size_t *len, size_t *cap, const char *text,
                             size_t text_len)
{
    if (*len + text_len + 1 > *cap) {
        size_t new_cap = *cap ? *cap : 64;
        char *tmp;

        while (new_cap < *len + text_len + 1) {
            new_cap *= 2;
        }
        tmp = realloc(*buf, new_cap);
        if (tmp == NULL) {
            fprintf(stderr, "Error: out of memory while accumulating "
                    "streamed response\n");
            ctx->oom = 1;
            return -1;
        }
        *buf = tmp;
        *cap = new_cap;
    }
    memcpy(*buf + *len, text, text_len);
    *len += text_len;
    (*buf)[*len] = '\0';
    return 0;
}

/**
 * @brief Appends a delta to the accumulated stream content.
 *
 * @return 0 on success, -1 on allocation failure (ctx->oom is set).
 */
static int stream_accumulate(struct stream_ctx *ctx, const char *text,
                             size_t text_len)
{
    return stream_buf_append(ctx, &ctx->content, &ctx->len, &ctx->cap,
                             text, text_len);
}

/**
 * @brief Appends a delta to the accumulated stream reasoning.
 *
 * @return 0 on success, -1 on allocation failure (ctx->oom is set).
 */
static int stream_accumulate_reasoning(struct stream_ctx *ctx,
                                       const char *text, size_t text_len)
{
    return stream_buf_append(ctx, &ctx->reasoning, &ctx->reasoning_len,
                             &ctx->reasoning_cap, text, text_len);
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
            ctx->on_delta(text, 0, ctx->user_data);
        }
    }

    // Reasoning deltas (reasoning_content, or the "reasoning" spelling
    // used by some providers): accumulate and forward with the flag set.
    if (cJSON_IsObject(delta)) {
        cJSON *reasoning = cJSON_GetObjectItem(delta, "reasoning_content");

        if (!cJSON_IsString(reasoning))
            reasoning = cJSON_GetObjectItem(delta, "reasoning");
        if (cJSON_IsString(reasoning) && reasoning->valuestring != NULL &&
            reasoning->valuestring[0] != '\0') {
            text = reasoning->valuestring;
            if (stream_accumulate_reasoning(ctx, text, strlen(text)) == 0 &&
                ctx->on_delta != NULL) {
                ctx->on_delta(text, 1, ctx->user_data);
            }
        }
    }

    // Accumulate streamed tool call fragments (delta.tool_calls).
    if (cJSON_IsObject(delta)) {
        cJSON *frag = cJSON_GetObjectItem(delta, "tool_calls");

        if (cJSON_IsArray(frag)) {
            cJSON *item = NULL;
            cJSON_ArrayForEach(item, frag) {
                if (cJSON_IsObject(item) &&
                    stream_accumulate_tool_call(ctx, item) != 0)
                    break; // ctx->oom is set; stop accumulating
            }
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
            // Transfer any accumulated reasoning to the response.
            if (response->content != NULL && ctx.reasoning != NULL) {
                response->reasoning = ctx.reasoning;
                ctx.reasoning = NULL; // ownership transferred
            }
            // Transfer any accumulated tool calls to the response.
            if (response->content != NULL && ctx.tool_calls != NULL) {
                response->tool_calls = ctx.tool_calls;
                response->tool_call_count = ctx.tool_call_count;
                ctx.tool_calls = NULL; // ownership transferred
                ctx.tool_call_count = 0;
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
    free(ctx.reasoning);
    openai_tool_calls_free(ctx.tool_calls, ctx.tool_call_count);
    return status;
}

void openai_response_free(openai_response_t *response) {
    if (response && response->content) {
        free(response->content);
        response->content = NULL;
    }
    if (response && response->reasoning) {
        free(response->reasoning);
        response->reasoning = NULL;
    }
    if (response && response->tool_calls) {
        openai_tool_calls_free(response->tool_calls,
                               response->tool_call_count);
        response->tool_calls = NULL;
        response->tool_call_count = 0;
    }
}
