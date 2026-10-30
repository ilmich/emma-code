#ifndef COMMANDS_H
#define COMMANDS_H

#include <stddef.h>

/** Outcome of commands_dispatch(): what the caller should do next. */
typedef enum {
	CMD_NOT_COMMAND, /**< input was not a registered command */
	CMD_HANDLED,     /**< command executed, feedback describes the result */
	CMD_EXIT         /**< command requests application exit */
} cmd_result_t;

/** Parsed invocation passed to command handlers. */
typedef struct cmd_invocation {
	const char *name; /**< command name without the leading '/' */
	const char *args; /**< arguments following the name, "" if none */
} cmd_invocation_t;

/**
 * Signature of a command handler.
 *
 * Writes a human-readable description of the result into feedback
 * (feedback may be NULL; feedback_len is its size). Returns the dispatch
 * outcome: CMD_HANDLED on success or CMD_EXIT to request application exit.
 */
typedef cmd_result_t (*cmd_handler_fn)(const cmd_invocation_t *inv,
				       void *userdata, char *feedback,
				       size_t feedback_len);

/**
 * Initializes the command registry and registers the built-in commands.
 * Returns 0 on success, -1 on failure (registry left unusable).
 */
int commands_init(void);

/** Releases all registry resources. Safe to call twice. */
void commands_free(void);

/**
 * Registers a command. name and help are copied into the registry;
 * userdata is opaque and stays owned by the caller. Duplicate names are
 * rejected.
 * Returns 0 on success, -1 on invalid arguments, duplicate name or OOM.
 */
int commands_register(const char *name, cmd_handler_fn fn,
		      const char *help, void *userdata);

/**
 * Parses input and dispatches a registered command if the input starts
 * with '/'. Writes a human-readable result description into feedback.
 * Returns CMD_NOT_COMMAND when input is NULL, not a command or unknown.
 */
cmd_result_t commands_dispatch(const char *input, char *feedback,
			       size_t feedback_len);

#endif /* COMMANDS_H */
