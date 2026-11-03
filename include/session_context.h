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

/**
 * @brief Adds a tool result message (role "tool") answering the tool call
 *        identified by tool_call_id. Handles resizing and truncation like
 *        session_context_add_message().
 * @param context Pointer to the session context.
 * @param tool_call_id The id of the tool call being answered.
 * @param content The tool output (text) handed back to the model.
 * @return 0 on success, -1 on failure (e.g., memory allocation).
 */
int session_context_add_tool_result(session_context_t *context,
                                    const char *tool_call_id,
                                    const char *content);

/**
 * @brief Adds an assistant message that requests tool executions. The
 *        tool_calls array is deep-copied; the caller keeps the ownership
 *        of its own copy. content may be NULL (tool-only message).
 * @param context Pointer to the session context.
 * @param content Optional assistant text accompanying the calls.
 * @param tool_calls Array of requested tool calls.
 * @param tool_call_count Number of entries in tool_calls (must be > 0).
 * @return 0 on success, -1 on failure (e.g., memory allocation).
 */
int session_context_add_assistant_tool_calls(session_context_t *context,
                                             const char *content,
                                             const openai_tool_call_t *tool_calls,
                                             size_t tool_call_count);

#endif // SESSION_CONTEXT_H