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
} openai_config_t;

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
 * @brief Frees any dynamically allocated memory within the openai_response_t structure.
 *
 * This is necessary because the content string is dynamically allocated by the client.
 *
 * @param response Pointer to the response structure to free.
 */
void openai_response_free(openai_response_t *response);

#endif // OPENAI_CLIENT_H