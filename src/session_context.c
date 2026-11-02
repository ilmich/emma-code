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
 * @brief Initializes a new session context.
 */
int session_context_init(session_context_t *context, size_t max_messages) {
    if (context == NULL) return -1;
    if (max_messages == 0) return -1; // A zero limit would truncate every message

    context->count = 0;
    context->capacity = INITIAL_CAPACITY;
    context->max_messages = max_messages;

    // Allocate initial memory for the message array
    context->messages = (openai_message_t*)malloc(context->capacity * sizeof(openai_message_t));
    if (context->messages == NULL) {
        return -1; // Allocation failure
    }

    // Initialize pointers and fields
    for (size_t i = 0; i < context->capacity; ++i) {
        context->messages[i].role = NULL;
        context->messages[i].content = NULL;
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
		free(context->messages[i].role);
		free(context->messages[i].content);
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
 * @brief Adds a new message to the session context. Handles dynamic resizing and truncation.
 */
int session_context_add_message(session_context_t *context, const char *role, const char *content) {
    if (context == NULL || role == NULL || content == NULL) {
        return -1;
    }

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

	free(context->messages[victim].role);
	free(context->messages[victim].content);

	// Shift all messages after the victim left by one.
        for (size_t i = victim; i + 1 < context->count; ++i) {
            context->messages[i] = context->messages[i+1];
        }
        context->count--;
    }

    // 3. Prepare and Add the New Message
    // We must duplicate the strings because the input pointers (role, content) might go out of scope
    // before the context is destroyed.
    openai_message_t* new_msg = &context->messages[context->count];

    new_msg->role = safe_strdup(role);
    new_msg->content = safe_strdup(content);

    if (new_msg->role == NULL || new_msg->content == NULL) {
        free(new_msg->role);
        free(new_msg->content);
        return -1;
    }

    context->count++;
    return 0;
}
