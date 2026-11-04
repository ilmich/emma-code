/**
 * @file openai_client.h
 * @brief Defines the interface for the OpenAI Chat Completion client.
 */

#ifndef OPENAI_CLIENT_H
#define OPENAI_CLIENT_H

#include <stddef.h>

/**
 * @brief Structure to hold the configuration for the OpenAI client.
 */
typedef struct {
    char* api_key;
    char* base_url; // e.g., "https://api.openai.com/v1"
    char* model_name;
    double temperature;
    int stream;     /**< nonzero: request a streamed (SSE) completion */
    char* tools_json; /**< optional: serialized JSON array of tool
                           definitions ({type:"function",function:{...}})
                           announced to the model; owned by the caller,
                           may be NULL */
} openai_config_t;

/**
 * @brief A single tool call requested by the assistant.
 *
 * arguments is the raw JSON string produced by the model (may be ""
 * when the function takes no parameters).
 */
typedef struct {
    char* id;        ///< tool call id (e.g. "call_abc123")
    char* name;      ///< function name
    char* arguments; ///< function arguments as a JSON string
} openai_tool_call_t;

/**
 * @brief Callback invoked for each streamed delta of the assistant content
 *        or reasoning.
 *
 * Called synchronously by openai_chat_completion_stream() on the same
 * thread, in arrival order. The text is NOT owned by the callback and is
 * only valid for the duration of the call.
 *
 * @param text NUL-terminated delta text (possibly empty).
 * @param is_reasoning Nonzero when the delta carries reasoning text
 *                     (delta.reasoning_content / delta.reasoning), zero
 *                     for regular assistant content.
 * @param user_data The user_data pointer passed to the stream function.
 */
typedef void (*openai_delta_callback_t)(const char* text, int is_reasoning,
                                       void* user_data);

/**
 * @brief Structure representing a single message in the chat history.
 *
 * Plain messages use only role and content. Assistant messages that
 * request tool executions carry a non-NULL tool_calls array (content may
 * be NULL). Tool results use role "tool" with tool_call_id set to the id
 * of the call being answered.
 */
typedef struct {
    char* role;    ///< "system", "user", "assistant" or "tool".
    char* content; ///< The message content (may be NULL for tool calls).
    char* tool_call_id; ///< role "tool": id of the answered call (or NULL).
    openai_tool_call_t* tool_calls; ///< assistant: requested calls (or NULL).
    size_t tool_call_count;         ///< number of entries in tool_calls.
} openai_message_t;

/**
 * @brief Structure to hold the response from the chat completion API.
 */
typedef struct {
    char* content; ///< The text content of the assistant's response (or NULL).
    char* reasoning; ///< Reasoning text (reasoning_content/reasoning) or NULL.
    openai_tool_call_t* tool_calls; ///< requested tool calls (or NULL).
    size_t tool_call_count;         ///< number of entries in tool_calls.
} openai_response_t;

/**
 * @brief Frees an array of tool calls (tolerates NULL).
 *
 * @param calls Array to free (may be NULL).
 * @param count Number of entries in the array.
 */
void openai_tool_calls_free(openai_tool_call_t *calls, size_t count);

/**
 * @brief Frees the dynamically allocated fields of a message (tolerates
 *        NULL). Does not free the structure itself.
 *
 * @param msg Message whose fields are released.
 */
void openai_message_free(openai_message_t *msg);

/**
 * @brief Performs a non-streaming chat completion request to OpenAI.
 *
 * @param config The configuration including API key and base URL.
 * @param messages Array of messages in the conversation.
 * @param message_count The number of messages in the array.
 * @param response Pointer to the structure where the response content will be stored.
 * @return 0 on success, 1 on failure.
 */
int openai_chat_completion(
    const openai_config_t *config,
    const openai_message_t *messages,
    size_t message_count,
    openai_response_t *response
);

/**
 * @brief Performs a chat completion request, optionally streaming (SSE).
 *
 * When the server answers with "text/event-stream", each
 * choices[0].delta.content chunk is forwarded to on_delta() (with
 * is_reasoning = 0) as it arrives and the full accumulated text is stored
 * in response->content at the end. Reasoning deltas
 * (choices[0].delta.reasoning_content or choices[0].delta.reasoning) are
 * forwarded with is_reasoning = 1 and accumulated into response->reasoning
 * (NULL when the model sent none).
 * Streamed tool calls (delta.tool_calls fragments indexed by "index") are
 * accumulated per index: id and function name are taken from the first
 * fragment carrying them, argument fragments are concatenated, and the
 * complete calls end up in response->tool_calls.
 * If the server ignores "stream": true and answers with a plain JSON
 * body, the response is parsed like the non-streaming path (on_delta is
 * never called).
 *
 * Malformed or metadata-only chunks are ignored with a warning on stderr;
 * fatal errors (network, HTTP error status, out of memory) return 1.
 *
 * @param config The configuration including API key and base URL.
 * @param messages Array of messages in the conversation.
 * @param message_count The number of messages in the array.
 * @param on_delta Optional callback invoked per delta (may be NULL).
 * @param user_data Opaque pointer handed back to on_delta (may be NULL).
 * @param response Pointer to the response structure where the full response
 *                 content and reasoning will be stored (owned by the caller,
 *                 free with openai_response_free()).
 * @return 0 on success, 1 on failure.
 */
int openai_chat_completion_stream(
    const openai_config_t *config,
    const openai_message_t *messages,
    size_t message_count,
    openai_delta_callback_t on_delta,
    void *user_data,
    openai_response_t *response
);

/**
 * @brief Frees any dynamically allocated memory within the openai_response_t structure.
 *
 * This is necessary because the content string is dynamically allocated by the client.
 *
 * @param response Pointer to the response structure to free.
 */
void openai_response_free(openai_response_t *response);

#endif // OPENAI_CLIENT_H
