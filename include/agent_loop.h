/**
 * @file agent_loop.h
 * @brief Agentic chat turn: sends a user message with session context to
 *        the OpenAI Chat Completion API and returns the raw assistant reply.
 *
 * The module owns nothing: config, session and TUI are borrowed from the
 * caller (main.c). It deliberately knows nothing about slash commands or
 * Lua scripting: command dispatch and the pre_message/post_message hooks
 * stay in main.c, which calls agent_loop_handle_message() only for real
 * chat messages.
 */

#ifndef AGENT_LOOP_H
#define AGENT_LOOP_H

#include "openai_client.h"
#include "session_context.h"
#include "tui.h"

/** Outcome of agent_loop_handle_message(): what the caller should do next. */
typedef enum {
	AGENT_MSG_OK,    /**< reply obtained; *reply is caller-owned (free()) */
	AGENT_MSG_ERROR, /**< completion failed; error already shown in TUI */
	AGENT_MSG_EXIT   /**< fatal error (session full): caller must stop */
} agent_msg_result_t;

/** Agentic loop context. All pointers are borrowed, never freed. */
typedef struct agent_loop {
	openai_config_t *config;    /**< OpenAI client configuration (borrowed) */
	session_context_t *session; /**< conversation history (borrowed) */
	tui_t *tui;                 /**< terminal UI, may be NULL (borrowed) */
} agent_loop_t;

/**
 * Handles a single user message: stores it in the session history, shows it
 * in the TUI, performs the chat completion request and returns the raw
 * assistant reply.
 *
 * Takes ownership of input (a string allocated by tui_read_input()) and
 * always frees it, on every path. On success *reply receives a
 * malloc'ed copy of the assistant content, owned by the caller.
 * On AGENT_MSG_ERROR the failure message has already been rendered in the
 * TUI and *reply is NULL; the caller should skip to the next input.
 * On AGENT_MSG_EXIT (session full) the caller should stop the loop.
 *
 * @param loop  Loop context (config and session must be non-NULL; tui may
 *              be NULL, e.g. in unit tests).
 * @param input User message, owned by and freed by this function.
 * @param reply Receives the malloc'ed assistant reply on AGENT_MSG_OK,
 *              NULL otherwise.
 * @return AGENT_MSG_OK, AGENT_MSG_ERROR or AGENT_MSG_EXIT.
 */
agent_msg_result_t agent_loop_handle_message(agent_loop_t *loop, char *input,
					     char **reply);

#endif /* AGENT_LOOP_H */
