/**
 * @file commands.c
 * @brief Generic command registry: dynamic registration, '/' dispatch.
 *
 * Commands are registered at runtime through commands_register(); the
 * built-in /exit is registered by commands_init() through the same public
 * API a future scripting engine would use.
 */

#include "commands.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** One registered command in the registry. */
typedef struct {
	char *name;         /**< owned copy of the command name */
	char *help;         /**< owned copy of the help text, may be NULL */
	cmd_handler_fn fn;  /**< handler invoked on dispatch */
	void *userdata;     /**< opaque context owned by the registrant */
} cmd_entry_t;

static cmd_entry_t *registry = NULL;
static size_t registry_count = 0;
static size_t registry_cap = 0;
static int initialized = 0;

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	if (d != NULL)
		memcpy(d, s, n);
	return d;
}

/** Looks up a command by name; returns its entry or NULL. */
static cmd_entry_t *find_command(const char *name, size_t len)
{
	for (size_t i = 0; i < registry_count; i++) {
		if (strlen(registry[i].name) == len &&
		    strncmp(registry[i].name, name, len) == 0)
			return &registry[i];
	}
	return NULL;
}

/** Built-in handler for /exit: requests application exit. */
static cmd_result_t exit_handler(const cmd_invocation_t *inv, void *userdata,
				 char *feedback, size_t feedback_len)
{
	(void)inv;
	(void)userdata;
	if (feedback != NULL && feedback_len > 0)
		feedback[0] = '\0';
	return CMD_EXIT;
}

int commands_init(void)
{
	if (initialized)
		return 0;
	registry = NULL;
	registry_count = 0;
	registry_cap = 0;
	if (commands_register("exit", exit_handler, "exit emma", NULL) != 0)
		return -1;
	initialized = 1;
	return 0;
}

void commands_free(void)
{
	for (size_t i = 0; i < registry_count; i++) {
		free(registry[i].name);
		free(registry[i].help);
	}
	free(registry);
	registry = NULL;
	registry_count = 0;
	registry_cap = 0;
	initialized = 0;
}

int commands_register(const char *name, cmd_handler_fn fn,
		      const char *help, void *userdata)
{
	if (name == NULL || *name == '\0' || fn == NULL)
		return -1;
	for (const char *c = name; *c != '\0'; c++) {
		if (*c == ' ' || *c == '\t' || *c == '/')
			return -1;
	}
	if (find_command(name, strlen(name)) != NULL)
		return -1;

	cmd_entry_t *grown = realloc(registry,
				     (registry_count + 1) * sizeof(*registry));
	if (grown == NULL)
		return -1;
	registry = grown;

	char *name_copy = xstrdup(name);
	if (name_copy == NULL)
		return -1;
	char *help_copy = NULL;
	if (help != NULL) {
		help_copy = xstrdup(help);
		if (help_copy == NULL) {
			free(name_copy);
			return -1;
		}
	}

	registry[registry_count].name = name_copy;
	registry[registry_count].help = help_copy;
	registry[registry_count].fn = fn;
	registry[registry_count].userdata = userdata;
	registry_count++;
	return 0;
}

cmd_result_t commands_dispatch(const char *input, char *feedback,
			       size_t feedback_len)
{
	if (feedback != NULL && feedback_len > 0)
		feedback[0] = '\0';

	if (input == NULL || input[0] != '/')
		return CMD_NOT_COMMAND;
	if (!initialized || registry == NULL)
		return CMD_NOT_COMMAND;

	/* skip blanks between '/' and the command name */
	const char *p = input + 1;
	while (*p == ' ' || *p == '\t')
		p++;
	const char *name_start = p;
	while (*p != '\0' && *p != ' ' && *p != '\t')
		p++;
	size_t name_len = (size_t)(p - name_start);

	char namebuf[64];
	if (name_len == 0 || name_len >= sizeof namebuf)
		return CMD_NOT_COMMAND;
	memcpy(namebuf, name_start, name_len);
	namebuf[name_len] = '\0';

	cmd_entry_t *cmd = find_command(namebuf, name_len);
	if (cmd == NULL) {
		if (feedback != NULL && feedback_len > 0)
			snprintf(feedback, feedback_len,
				 "Unknown command: %.*s", (int)name_len,
				 name_start);
		return CMD_NOT_COMMAND;
	}

	/* arguments: everything after the name, leading blanks skipped */
	const char *args = p;
	while (*args == ' ' || *args == '\t')
		args++;

	cmd_invocation_t inv = { namebuf, args };
	return cmd->fn(&inv, cmd->userdata, feedback, feedback_len);
}
