#include "test.h"

#include "commands.h"

#include <string.h>

static char last_args[128];
static char last_name[64];

static cmd_result_t capture_handler(const cmd_invocation_t *inv,
				    void *userdata, char *feedback,
				    size_t feedback_len)
{
	(void)userdata;
	last_name[0] = '\0';
	last_args[0] = '\0';
	snprintf(last_name, sizeof last_name, "%s", inv->name);
	snprintf(last_args, sizeof last_args, "%s", inv->args);
	if (feedback != NULL && feedback_len > 0)
		snprintf(feedback, feedback_len, "captured");
	return CMD_HANDLED;
}

static cmd_result_t exit_handler_mock(const cmd_invocation_t *inv,
				      void *userdata, char *feedback,
				      size_t feedback_len)
{
	(void)inv;
	(void)userdata;
	if (feedback != NULL && feedback_len > 0)
		feedback[0] = '\0';
	return CMD_EXIT;
}

int main(void)
{
	char fb[128];

	/* Regression: a command registered before init() must survive init
	 * (init used to reset the registry and leak the entry). */
	TEST_CHECK_INT_EQ(
		commands_register("early", capture_handler, "early bird", NULL),
		0);
	TEST_CHECK_INT_EQ(commands_init(), 0);
	TEST_CHECK_INT_EQ(commands_dispatch("/early x", fb, sizeof fb),
			  CMD_HANDLED);
	TEST_CHECK_STR_EQ(last_name, "early");

	/* init registers the built-in /exit command */
	TEST_CHECK_INT_EQ(commands_init(), 0);
	TEST_CHECK_INT_EQ(commands_dispatch("/exit", fb, sizeof fb), CMD_EXIT);

	/* unknown command: not a command, descriptive feedback */
	TEST_CHECK_INT_EQ(commands_dispatch("/does_not_exist", fb, sizeof fb),
			  CMD_NOT_COMMAND);
	TEST_CHECK(fb[0] != '\0');

	/* input that does not start with '/' */
	TEST_CHECK_INT_EQ(commands_dispatch("plain chat", fb, sizeof fb),
			  CMD_NOT_COMMAND);
	TEST_CHECK_INT_EQ(commands_dispatch(NULL, fb, sizeof fb),
			  CMD_NOT_COMMAND);

	/* NULL handler is rejected */
	TEST_CHECK(commands_register("bad", NULL, NULL, NULL) != 0);

	/* duplicate registration is rejected */
	TEST_CHECK_INT_EQ(
		commands_register("greet", capture_handler, "say hi", NULL),
		0);
	TEST_CHECK_INT_EQ(
		commands_register("greet", capture_handler, NULL, NULL) != 0,
		1);

	/* registered command: handler receives name and args */
	TEST_CHECK_INT_EQ(commands_dispatch("/greet one   two", fb, sizeof fb),
			  CMD_HANDLED);
	TEST_CHECK_STR_EQ(last_name, "greet");
	TEST_CHECK_STR_EQ(last_args, "one   two");
	TEST_CHECK_STR_EQ(fb, "captured");

	/* no args: handler receives empty args string */
	TEST_CHECK_INT_EQ(commands_dispatch("/greet", fb, sizeof fb),
			  CMD_HANDLED);
	TEST_CHECK_STR_EQ(last_args, "");

	/* leading blanks after the command name are skipped */
	TEST_CHECK_INT_EQ(commands_dispatch("/greet    x", fb, sizeof fb),
			  CMD_HANDLED);
	TEST_CHECK_STR_EQ(last_args, "x");

	/* commands are case-sensitive */
	TEST_CHECK_INT_EQ(commands_dispatch("/GREET", fb, sizeof fb),
			  CMD_NOT_COMMAND);

	/* no partial matches on name prefixes */
	TEST_CHECK_INT_EQ(commands_dispatch("/gree", fb, sizeof fb),
			  CMD_NOT_COMMAND);
	TEST_CHECK_INT_EQ(commands_dispatch("/greetx", fb, sizeof fb),
			  CMD_NOT_COMMAND);

	/* unknown command gets descriptive feedback */
	TEST_CHECK_INT_EQ(commands_dispatch("/nope", fb, sizeof fb),
			  CMD_NOT_COMMAND);
	TEST_CHECK(fb[0] != '\0');

	/* defensive: dispatch on a freed registry is safe */
	commands_free();
	TEST_CHECK_INT_EQ(commands_dispatch("/exit", fb, sizeof fb),
			  CMD_NOT_COMMAND);
	TEST_CHECK_INT_EQ(commands_dispatch("/greet", fb, sizeof fb),
			  CMD_NOT_COMMAND);

	/* re-init works and re-registers the built-ins */
	TEST_CHECK_INT_EQ(commands_init(), 0);
	TEST_CHECK_INT_EQ(commands_dispatch("/exit", fb, sizeof fb),
			  CMD_EXIT);

	/* double free is safe */
	commands_free();
	commands_free();

	if (test__failures == 0)
		printf("All command tests passed\n");
	return test__failures ? 1 : 0;
}
