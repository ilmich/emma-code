/**
 * @file agent_loop.c
 * @brief Agentic chat turn: session update, chat completion, raw reply.
 *
 * Extracted from the inline loop in main.c. Handles exactly one user
 * message per call; command dispatch and the Lua pre_message/post_message
 * hooks deliberately remain in main.c.
 */

#include "agent_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** strdup wrapper returning NULL instead of crashing on OOM. */
static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	if (d != NULL)
		memcpy(d, s, n);
	return d;
}

agent_msg_result_t agent_loop_handle_message(agent_loop_t *loop, char *input,
					     char **reply)
{
	if (reply != NULL)
		*reply = NULL;

	if (loop == NULL || loop->config == NULL || loop->session == NULL ||
	    input == NULL || reply == NULL) {
		free(input);
		return AGENT_MSG_ERROR;
	}

	/* Store the user message first: a full session is fatal and must
	 * abort the loop exactly like the previous inline `break` did. */
	if (session_context_add_message(loop->session, "user", input) != 0) {
		free(input);
		return AGENT_MSG_EXIT;
	}

	tui_append_message(loop->tui, "user", input);
	free(input);
	input = NULL;

	tui_set_status(loop->tui, "Thinking...");

	openai_response_t response = {0};
	int result = openai_chat_completion(loop->config,
					    loop->session->messages,
					    loop->session->count, &response);

	tui_set_status(loop->tui, NULL);

	if (result != 0 || response.content == NULL) {
		tui_append_message(loop->tui, "assistant",
				   "[Error: failed to get response]");
		openai_response_free(&response);
		return AGENT_MSG_ERROR;
	}

	/* The response buffer is owned here; hand out an owned copy so the
	 * caller never touches openai_client internals. */
	char *copy = xstrdup(response.content);

	if (copy == NULL) {
		fprintf(stderr, "agent_loop: out of memory\n");
		tui_append_message(loop->tui, "assistant",
				   "[Error: failed to get response]");
		openai_response_free(&response);
		return AGENT_MSG_ERROR;
	}
	*reply = copy;

	openai_response_free(&response);
	return AGENT_MSG_OK;
}
