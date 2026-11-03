/**
 * @file agent_loop.c
 * @brief Agentic chat turn: session update, chat completion, tool call
 *        rounds, raw reply.
 *
 * Extracted from the inline loop in main.c. Handles exactly one user
 * message per call; command dispatch, the Lua pre_message/post_message
 * hooks and every TUI interaction deliberately remain in main.c. This
 * module is a pure session + API engine and does not include tui.h. Tool
 * execution policy (lookup, approval, run) lives behind the
 * on_tool_call dispatcher callback owned by the caller.
 */

#include "agent_loop.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Performs one completion request (streaming or not) into response.
 *
 * @return 0 on success, nonzero on failure.
 */
static int run_completion(const agent_loop_t *loop, openai_response_t *response)
{
	if (loop->config->stream) {
		return openai_chat_completion_stream(loop->config,
						     loop->session->messages,
						     loop->session->count,
						     loop->on_delta,
						     loop->delta_user_data,
						     response);
	}
	return openai_chat_completion(loop->config,
				      loop->session->messages,
				      loop->session->count,
				      response);
}

/**
 * @brief Executes one requested tool call through the dispatcher and
 *        stores the result in the session as a role "tool" message.
 *
 * Dispatch failures (unknown tool, bad arguments, handler errors) are
 * never fatal: the error text is stored as the tool result so the model
 * can react. A session-store failure is fatal.
 *
 * @return 0 on success, 1 on fatal store failure.
 */
static int run_tool_call(const agent_loop_t *loop,
			 const openai_tool_call_t *call)
{
	char *result = NULL;
	const char *result_text;
	int dispatch_failed;
	int stored;

	dispatch_failed = loop->on_tool_call(call->name != NULL ? call->name : "",
					     call->arguments != NULL
						 ? call->arguments : "",
					     &result, loop->tool_user_data);
	/* The dispatcher always produces a text result (execution failures
	 * are conveyed through the text, not the return value); only a
	 * malloc failure leaves it NULL. */
	result_text = result != NULL ? result
				     : "[tool error: out of memory]";
	if (dispatch_failed != 0 && result == NULL)
		result_text = "[tool error: dispatcher failure]";

	stored = session_context_add_tool_result(loop->session,
						 call->id != NULL ? call->id : "",
						 result_text);
	free(result);
	return stored != 0 ? 1 : 0;
}

agent_msg_result_t agent_loop_handle_message(agent_loop_t *loop, char *input,
					     char **reply)
{
	openai_response_t response = {0};
	agent_msg_result_t result = AGENT_MSG_ERROR;
	int max_rounds;
	char *copy = NULL;

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

	max_rounds = loop->max_tool_rounds > 0
			 ? loop->max_tool_rounds
			 : AGENT_LOOP_MAX_ROUNDS_DEFAULT;

	for (int round = 0; round <= max_rounds; round++) {
		int api_status = run_completion(loop, &response);

		if (api_status != 0)
			goto cleanup; /* ERROR: caller renders the failure */

		if (response.tool_call_count > 0 && loop->on_tool_call != NULL) {
			/* The assistant requested tool executions: store its
			 * message, run every call, feed the results back and
			 * loop for the next completion. */
			if (session_context_add_assistant_tool_calls(
				loop->session, response.content,
				response.tool_calls,
				response.tool_call_count) != 0) {
				result = AGENT_MSG_EXIT;
				goto cleanup;
			}
			for (size_t i = 0; i < response.tool_call_count; ++i) {
				if (run_tool_call(loop,
						  &response.tool_calls[i]) != 0) {
					result = AGENT_MSG_EXIT;
					goto cleanup;
				}
			}
			openai_response_free(&response);
			continue;
		}

		/* Plain text reply (or tool calls with no dispatcher):
		 * the response buffer is owned here; hand out an owned copy
		 * so the caller never touches openai_client internals. */
		copy = util_strdup(response.content != NULL
				       ? response.content
				       : "");
		if (copy == NULL) {
			fprintf(stderr, "agent_loop: out of memory\n");
			goto cleanup;
		}
		*reply = copy;
		copy = NULL; /* ownership transferred to *reply */
		result = AGENT_MSG_OK;
		goto cleanup;
	}

	/* Too many tool rounds without a final answer: surface a visible
	 * message instead of looping forever. */
	copy = util_strdup("[Error: tool round limit reached]");
	if (copy == NULL)
		goto cleanup;
	*reply = copy;
	copy = NULL; /* ownership transferred to *reply */
	result = AGENT_MSG_OK;

cleanup:
	openai_response_free(&response);
	free(copy);
	free(input);
	return result;
}
