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
} openai_config_t;

/**
 * @brief Callback invoked for each streamed delta of the assistant content.
 *
 * Called synchronously by openai_chat_completion_stream() on the same
 * thread, in arrival order. The text is NOT owned by the callback and is
 * only valid for the duration of the call.
 *
 * @param text NUL-terminated delta text (possibly empty).
 * @param user_data The user_data pointer passed to the stream function.
 */
typedef void (*openai_delta_callback_t)(const char* text, void* user_data);

/**
 * @brief Structure representing a single message in the chat history.
 */
typedef struct {
    char* role;    ///< "system", "user", or "assistant".
    char* content; ///< The message content.
} openai_message_t;

/**
 * @brief Structure to hold the response from the chat completion API.
 */
typedef struct {
    char* content; ///< The text content of the assistant's response.
} openai_response_t;

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
 * choices[0].delta.content chunk is forwarded to on_delta() as it arrives
 * and the full accumulated text is stored in response->content at the end.
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
 * @param response Pointer to the structure where the full response content
 *                 will be stored (owned by the caller, free with
 *                 openai_response_free()).
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