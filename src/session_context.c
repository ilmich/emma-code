#include "session_context.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h> // For potential logging/debugging

// Default initial capacity
#define INITIAL_CAPACITY 4

/**
 * @brief Helper function to safely duplicate a string.
 */
static char* safe_strdup(const char* s) {
    if (s == NULL) return NULL;
    char* new_s = (char*)malloc(strlen(s) + 1);
    if (new_s) {
        strcpy(new_s, s);
    }
    return new_s;
}

/**
 * @brief Releases the fields of one stored message (deep free).
 */
static void message_clear(openai_message_t *msg) {
    openai_message_free(msg);
}

/**
 * @brief Initializes a new session context.
 */
int session_context_init(session_context_t *context, size_t max_messages) {
    if (context == NULL) return -1;
    if (max_messages == 0) return -1; // A zero limit would truncate every message

    context->count = 0;
    context->capacity = INITIAL_CAPACITY;
    context->max_messages = max_messages;

    // Allocate initial memory for the message array
    context->messages = (openai_message_t*)calloc(context->capacity, sizeof(openai_message_t));
    if (context->messages == NULL) {
        return -1; // Allocation failure
    }

    return 0;
}

/**
 * @brief Cleans up and frees all memory associated with the session context.
 */
void session_context_free(session_context_t *context) {
    if (context != NULL) {
	// Free content and role strings for every message
	for (size_t i = 0; i < context->count; ++i) {
	    message_clear(&context->messages[i]);
	}

	// Free the array itself
	free(context->messages);

	// Reset context state
	context->messages = NULL;
	context->count = 0;
	context->capacity = 0;
	context->max_messages = 0;
    }
}

/**
 * @brief Makes room for one more message: grows the array when needed and
 *        applies the truncation policy (drop the oldest non-system
 *        message) when the hard limit is reached.
 *
 * @return 0 on success (room available), -1 on allocation failure.
 */
static int make_room(session_context_t *context) {
    // 1. Grow first: on allocation failure the session is left untouched.
    //    The new message always needs room, truncation or not.
    if (context->count + 1 >= context->capacity) {
        size_t new_capacity = context->capacity * 2;
        openai_message_t* new_messages = (openai_message_t*)realloc(context->messages, new_capacity * sizeof(openai_message_t));

        if (new_messages == NULL) {
            // Failed to reallocate, prevent memory leak by returning failure
            return -1;
        }

        context->messages = new_messages;
        context->capacity = new_capacity;
    }

    // 2. Check for Truncation (Context Limit)
    // Guard against unsigned underflow: only truncate when there is at least
    // one message to drop.
    if (context->count > 0 && context->count >= context->max_messages) {
        // Truncation strategy: drop the oldest non-system message so that a
        // system prompt (if present at the top) is never evicted.
        size_t victim = 0;
        while (victim < context->count &&
               context->messages[victim].role != NULL &&
               strcmp(context->messages[victim].role, "system") == 0) {
            victim++;
        }
        if (victim >= context->count) {
            victim = 0; // All messages are system prompts: drop the oldest
        }

	message_clear(&context->messages[victim]);

	// Shift all messages after the victim left by one.
        for (size_t i = victim; i + 1 < context->count; ++i) {
            context->messages[i] = context->messages[i+1];
        }
        context->count--;
    }

    return 0;
}

/**
 * @brief Adds a new message to the session context. Handles dynamic resizing and truncation.
 */
int session_context_add_message(session_context_t *context, const char *role, const char *content) {
    if (context == NULL || role == NULL || content == NULL) {
        return -1;
    }

    if (make_room(context) != 0) {
        return -1;
    }

    // 3. Prepare and Add the New Message
    // We must duplicate the strings because the input pointers (role, content) might go out of scope
    // before the context is destroyed. The slot is zeroed first: growth by
    // realloc does not initialize the new tool-related fields.
    openai_message_t* new_msg = &context->messages[context->count];

    memset(new_msg, 0, sizeof(*new_msg));
    new_msg->role = safe_strdup(role);
    new_msg->content = safe_strdup(content);

    if (new_msg->role == NULL || new_msg->content == NULL) {
        message_clear(new_msg);
        return -1;
    }

    context->count++;
    return 0;
}

/**
 * @brief Adds a tool result message (role "tool") answering the tool call
 *        identified by tool_call_id.
 */
int session_context_add_tool_result(session_context_t *context,
                                    const char *tool_call_id,
                                    const char *content) {
    if (context == NULL || tool_call_id == NULL || content == NULL) {
        return -1;
    }

    if (make_room(context) != 0) {
        return -1;
    }

    openai_message_t* new_msg = &context->messages[context->count];

    memset(new_msg, 0, sizeof(*new_msg));
    new_msg->role = safe_strdup("tool");
    new_msg->content = safe_strdup(content);
    new_msg->tool_call_id = safe_strdup(tool_call_id);

    if (new_msg->role == NULL || new_msg->content == NULL ||
        new_msg->tool_call_id == NULL) {
        message_clear(new_msg);
        return -1;
    }

    context->count++;
    return 0;
}

/**
 * @brief Adds an assistant message that requests tool executions. The
 *        tool_calls array is deep-copied; the caller keeps ownership of
 *        its own copy.
 */
int session_context_add_assistant_tool_calls(session_context_t *context,
                                             const char *content,
                                             const openai_tool_call_t *tool_calls,
                                             size_t tool_call_count) {
    if (context == NULL || tool_calls == NULL || tool_call_count == 0 ||
        (content == NULL && tool_call_count == 0)) {
        return -1;
    }

    if (make_room(context) != 0) {
        return -1;
    }

    openai_message_t* new_msg = &context->messages[context->count];

    memset(new_msg, 0, sizeof(*new_msg));
    new_msg->role = safe_strdup("assistant");
    new_msg->content = safe_strdup(content != NULL ? content : "");
    if (new_msg->role == NULL || new_msg->content == NULL) {
        message_clear(new_msg);
        return -1;
    }

    new_msg->tool_calls = (openai_tool_call_t*)calloc(tool_call_count,
                                                      sizeof(openai_tool_call_t));
    if (new_msg->tool_calls == NULL) {
        message_clear(new_msg);
        return -1;
    }
    for (size_t i = 0; i < tool_call_count; ++i) {
        if (tool_calls[i].id != NULL) {
            new_msg->tool_calls[i].id = safe_strdup(tool_calls[i].id);
            if (new_msg->tool_calls[i].id == NULL)
                goto oom;
        }
        if (tool_calls[i].name != NULL) {
            new_msg->tool_calls[i].name = safe_strdup(tool_calls[i].name);
            if (new_msg->tool_calls[i].name == NULL)
                goto oom;
        }
        if (tool_calls[i].arguments != NULL) {
            new_msg->tool_calls[i].arguments = safe_strdup(tool_calls[i].arguments);
            if (new_msg->tool_calls[i].arguments == NULL)
                goto oom;
        }
    }
    new_msg->tool_call_count = tool_call_count;

    context->count++;
    return 0;

oom:
    message_clear(new_msg);
    return -1;
}
