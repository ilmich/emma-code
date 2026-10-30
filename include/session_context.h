/**
 * @file session_context.h
 * @brief Defines the structure and functions for managing conversation history (session context).
 */

#ifndef SESSION_CONTEXT_H
#define SESSION_CONTEXT_H

#include <stddef.h>
#include "openai_client.h" // Dependency on the message structure

/**
 * @brief Structure representing the entire session context.
 */
typedef struct {
    openai_message_t* messages; ///< Dynamic array of messages in the conversation.
    size_t count;             ///< Current number of messages in the context.
    size_t capacity;         ///< Maximum number of messages the array can hold.
    size_t max_messages;    ///< Hard limit on context size (e.g., 10 messages).
} session_context_t;

/**
 * @brief Initializes a new session context.
 * @param context Pointer to the context structure to initialize.
 * @param max_messages The maximum number of messages allowed in the context.
 * @return 0 on success, -1 on failure (e.g., memory allocation).
 */
int session_context_init(session_context_t *context, size_t max_messages);

/**
 * @brief Cleans up and frees all memory associated with the session context.
 * @param context Pointer to the context structure to destroy.
 */
void session_context_free(session_context_t *context);

/**
 * @brief Adds a new message to the session context. Handles dynamic resizing and truncation.
 * @param context Pointer to the session context.
 * @param role The role of the message ("user", "assistant", "system").
 * @param content The content of the message.
 * @return 0 on success, -1 on failure (e.g., memory allocation or context full).
 */
int session_context_add_message(session_context_t *context, const char *role, const char *content);


#endif // SESSION_CONTEXT_H