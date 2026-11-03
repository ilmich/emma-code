/**
 * @file agent_loop.c
 * @brief Agentic chat turn: session update, chat completion, raw reply.
 *
 * Extracted from the inline loop in main.c. Handles exactly one user
 * message per call; command dispatch, the Lua pre_message/post_message
 * hooks and every TUI interaction deliberately remain in main.c. This
 * module is a pure session + API engine and does not include tui.h.
 */

#include "agent_loop.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

agent_msg_result_t agent_loop_handle_message(agent_loop_t *loop, char *input,
					     char **reply)
{
	openai_response_t response = {0};
	agent_msg_result_t result = AGENT_MSG_ERROR;
	int api_status;

	if (reply != NULL)
		*reply = NULL;

	if (loop == NULL || loop->config == NULL || loop->session == NULL ||
	    input == NULL || reply == NULL)
		goto cleanup;

	/* Store the user message first: a full session is fatal and must
	 * abort the loop exactly like the previous inline `break` did. */
	if (session_context_add_message(loop->session, "user", input) != 0) {
		result = AGENT_MSG_EXIT;
		goto cleanup;
	}

	if (loop->config->stream) {
		api_status = openai_chat_completion_stream(loop->config,
							   loop->session->messages,
							   loop->session->count,
							   loop->on_delta,
							   loop->delta_user_data,
							   &response);
	} else {
		api_status = openai_chat_completion(loop->config,
						    loop->session->messages,
						    loop->session->count,
						    &response);
	}

	if (api_status != 0 || response.content == NULL)
		goto cleanup;

	/* The response buffer is owned here; hand out an owned copy so the
	 * caller never touches openai_client internals. */
	char *copy = util_strdup(response.content);

	if (copy == NULL) {
		fprintf(stderr, "agent_loop: out of memory\n");
		goto cleanup;
	}
	*reply = copy;
	result = AGENT_MSG_OK;

cleanup:
	openai_response_free(&response);
	free(input);
	return result;
}
