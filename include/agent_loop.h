/**
 * @file agent_loop.h
 * @brief Agentic chat turn: sends a user message with session context to
 *        the OpenAI Chat Completion API and returns the raw assistant reply.
 *
 * The module owns nothing: config and session are borrowed from the caller
 * (main.c). It deliberately knows nothing about slash commands, Lua
 * scripting or the TUI: command dispatch and the pre_message/post_message
 * hooks stay in main.c, which calls agent_loop_handle_message() only for
 * real chat messages and renders everything (user message, status, errors,
 * streamed deltas) itself.
 */

#ifndef AGENT_LOOP_H
#define AGENT_LOOP_H

#include "openai_client.h"
#include "session_context.h"
#include "interrupt.h"

/** Default cap on tool-call rounds per message (loop safety guard). */
#define AGENT_LOOP_MAX_ROUNDS_DEFAULT 10

/** Outcome of agent_loop_handle_message(): what the caller should do next. */
typedef enum {
	AGENT_MSG_OK,    /**< reply obtained; *reply is caller-owned (free()) */
	AGENT_MSG_ERROR, /**< completion failed; *reply is NULL; the caller
			      owns all error/status rendering */
	AGENT_MSG_EXIT,  /**< fatal error (session full): caller must stop */
	AGENT_MSG_INTERRUPTED /**< the user pressed ESC during the completion
			      (streaming or not): *reply holds the partial
			      streamed text (NULL when none, e.g. non-streaming
			      or nothing arrived); the caller owns it and decides
			      about session storage and display. No tool calls
			      from the interrupted response are executed. */
} agent_msg_result_t;

/**
 * Signature of the tool dispatcher invoked for every tool call requested
 * by the model. Implementations perform the lookup, any user approval and
 * the execution, and write a text result into *result (malloc, owned by
 * the caller of the dispatcher). The return value only signals malloc
 * failure of *result (nonzero); execution failures are conveyed through
 * the *result text, which is always handed back to the model.
 */
typedef int (*agent_tool_dispatch_fn)(const char *name,
				      const char *arguments_json,
				      char **result, void *user_data);

/** Agentic loop context. All pointers are borrowed, never freed. */
typedef struct agent_loop {
	openai_config_t *config;   /**< OpenAI client configuration (borrowed) */
	session_context_t *session; /**< conversation history (borrowed) */
	openai_delta_callback_t on_delta; /**< optional streamed-delta callback
					       (may be NULL; used only when
					       config->stream is nonzero) */
	void *delta_user_data;     /**< opaque pointer handed back to on_delta */
	agent_tool_dispatch_fn on_tool_call; /**< optional tool dispatcher
					      (may be NULL; when NULL,
					      requested tool calls are not
					      executed) */
	void *tool_user_data;      /**< opaque pointer handed to on_tool_call */
	int max_tool_rounds;       /**< cap on tool rounds per message
				        (<= 0: AGENT_LOOP_MAX_ROUNDS_DEFAULT) */
} agent_loop_t;

/**
 * Handles a single user message: stores it in the session history, performs
 * the chat completion request and returns the raw assistant reply. It
 * performs NO rendering: displaying the user message, the status line,
 * error messages and streamed deltas is entirely the caller's job.
 *
 * Takes ownership of input (a string allocated by tui_read_input()) and
 * always frees it, on every path. On success *reply receives a
 * malloc'ed copy of the assistant content, owned by the caller.
 * On AGENT_MSG_ERROR the completion failed, *reply is NULL and the caller
 * should render the error and skip to the next input.
 * On AGENT_MSG_EXIT (session full) the caller should stop the loop.
 *
 * @param loop  Loop context (config and session must be non-NULL).
 * @param input User message, owned by and freed by this function.
 * @param reply Receives the malloc'ed assistant reply on AGENT_MSG_OK,
 *              NULL otherwise.
 * @param reasoning Receives the malloc'ed reasoning text of the final
 *              reply (or NULL when the model sent none) on AGENT_MSG_OK;
 *              NULL always when @p reasoning is NULL. Owned by the caller.
 * @return AGENT_MSG_OK, AGENT_MSG_ERROR or AGENT_MSG_EXIT.
 */
agent_msg_result_t agent_loop_handle_message(agent_loop_t *loop, char *input,
					     char **reply, char **reasoning);

#endif /* AGENT_LOOP_H */
