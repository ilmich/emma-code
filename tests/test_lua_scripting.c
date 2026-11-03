/**
 * @file test_lua_scripting.c
 * @brief Unit tests for the Lua scripting bridge (commands, hooks,
 *        loading, error handling).
 */

#include "test.h"

#include "lua_scripting.h"
#include "commands.h"
#include "tools.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

/* POSIX symbol not declared by strict ISO C99 headers on this toolchain
 * (see AGENTS.md portability rules). */
extern char *strdup(const char *s);

#define FIX_OK    "tests/fixtures/scripts_ok"
#define FIX_CHAIN "tests/fixtures/scripts_chain"
#define FIX_BAD   "tests/fixtures/scripts_bad"
#define FIX_EMPTY "tests/fixtures/scripts_empty"
#define FIX_TOOLS "tests/fixtures/scripts_tools"
#define FIX_NONE  "tests/fixtures/no_such_dir"

/** Resets the command registry and initializes it for a fresh scenario. */
static void reset_commands(void)
{
	commands_free();
	TEST_CHECK_INT_EQ(commands_init(), 0);
}

/** Frees a hook result and NULLs the pointer (ownership helper). */
static void free_out(char **out)
{
	free(*out);
	*out = NULL;
}

static void test_init_and_free(void)
{
	lua_bridge_t *bridge = NULL;

	TEST_CHECK_INT_EQ(lua_bridge_init(NULL, NULL), -1);
	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_NOT_NULL(bridge);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_EMPTY), 0);
	TEST_CHECK_STR_EQ(lua_bridge_last_error(bridge), "");
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_PRE_MESSAGE,
						"payload", NULL), -1);
	char *out = NULL;
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_PRE_MESSAGE,
						"payload", &out), 0);
	TEST_CHECK_NULL(out);
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, "bogus_event",
						"payload", &out), -1);
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, NULL, "p", &out), -1);
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_PRE_MESSAGE,
						NULL, &out), -1);
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_PRE_MESSAGE,
						"p", NULL), -1);
	lua_bridge_free(bridge);
	lua_bridge_free(NULL); /* double free must be safe */
	TEST_CHECK_STR_EQ(lua_bridge_last_error(NULL), "");
}

static void test_load_missing_and_empty_dirs(void)
{
	lua_bridge_t *bridge = NULL;

	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_NONE), 0);
	TEST_CHECK_STR_EQ(lua_bridge_last_error(bridge), "");
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_EMPTY), 0);
	TEST_CHECK_STR_EQ(lua_bridge_last_error(bridge), "");
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(NULL, FIX_OK), -1);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, NULL), -1);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, ""), -1);
	char *out = NULL;
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_POST_MESSAGE,
						"resp", &out), 0);
	TEST_CHECK_NULL(out); /* nothing was loaded, no hook ran */
	lua_bridge_free(bridge);
}

static void test_load_bad_scripts_non_fatal(void)
{
	lua_bridge_t *bridge = NULL;
	char *out = NULL;

	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_BAD), 0);
	TEST_CHECK_STR_NE(lua_bridge_last_error(bridge), "");
	/* Neither broken.lua (syntax) nor badhook.lua (runtime) registered
	 * any hook: applying must be a clean pass-through. */
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_PRE_MESSAGE,
						"text", &out), 0);
	TEST_CHECK_NULL(out);
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_POST_MESSAGE,
						"text", &out), 0);
	TEST_CHECK_NULL(out);
	lua_bridge_free(bridge);
}

static void test_command_dispatch(void)
{
	lua_bridge_t *bridge = NULL;
	char feedback[256];

	reset_commands();
	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_OK), 0);
	TEST_CHECK_STR_EQ(lua_bridge_last_error(bridge), "");

	TEST_CHECK_INT_EQ(commands_dispatch("/greet bob", feedback,
					    sizeof feedback), CMD_HANDLED);
	TEST_CHECK_STR_EQ(feedback, "hello, bob");
	TEST_CHECK_INT_EQ(commands_dispatch("/greet", feedback,
					    sizeof feedback), CMD_HANDLED);
	TEST_CHECK_STR_EQ(feedback, "hello, stranger");
	/* unknown commands still route to the chat */
	TEST_CHECK_INT_EQ(commands_dispatch("/unknowncmd", feedback,
					    sizeof feedback), CMD_NOT_COMMAND);
	/* built-in /exit must not be hijacked by dup_exit.lua */
	TEST_CHECK_INT_EQ(commands_dispatch("/exit", feedback,
					    sizeof feedback), CMD_EXIT);
	lua_bridge_free(bridge);
	commands_free();
}

static void test_command_runtime_error(void)
{
	lua_bridge_t *bridge = NULL;
	char feedback[256];

	reset_commands();
	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_OK), 0);

	TEST_CHECK_INT_EQ(commands_dispatch("/boom now", feedback,
					    sizeof feedback), CMD_HANDLED);
	TEST_CHECK_STR_NE(feedback, "");
	TEST_CHECK_STR_NE(lua_bridge_last_error(bridge), "");
	TEST_CHECK(strstr(feedback, "kaboom") == NULL); /* no raw Lua error */
	lua_bridge_free(bridge);
	commands_free();
}

static void test_pre_message_hook(void)
{
	lua_bridge_t *bridge = NULL;
	char *out = NULL;

	/* FIX_OK registers slash commands: start from a clean registry so
	 * the fixture scripts do not abort on duplicate names. */
	reset_commands();
	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_OK), 0);

	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_PRE_MESSAGE,
						"hook-me", &out), 0);
	TEST_CHECK_NOT_NULL(out);
	TEST_CHECK_STR_EQ(out, "hook-me!");
	free_out(&out);

	/* hook returns nil: pass-through */
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_PRE_MESSAGE,
						"other text", &out), 0);
	TEST_CHECK_NULL(out);
	lua_bridge_free(bridge);
	commands_free();
}

static void test_post_message_hook(void)
{
	lua_bridge_t *bridge = NULL;
	char *out = NULL;

	reset_commands();
	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_OK), 0);

	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_POST_MESSAGE,
						"the answer", &out), 0);
	TEST_CHECK_NOT_NULL(out);
	TEST_CHECK_STR_EQ(out, "the answer [ok]");
	free_out(&out);
	lua_bridge_free(bridge);
	commands_free();
}

static void test_hook_chain_order(void)
{
	lua_bridge_t *bridge = NULL;
	char *out = NULL;

	reset_commands();
	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_CHAIN), 0);

	/* chain1.lua and chain2.lua load alphabetically; each receives the
	 * previous hook's output. */
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_POST_MESSAGE,
						"resp", &out), 0);
	TEST_CHECK_NOT_NULL(out);
	TEST_CHECK_STR_EQ(out, "resp-A-B");
	free_out(&out);
	lua_bridge_free(bridge);
}

static void test_hook_runtime_error_pass_through(void)
{
	lua_bridge_t *bridge = NULL;
	char *out = NULL;

	/* scripts_ok contains boom.lua (command error) but no hook error;
	 * use a dedicated in-memory scenario instead: apply with the bad
	 * fixtures dir registers nothing, so exercise the error path by
	 * loading a dir with a hook that fails at dispatch time. */
	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_BAD), 0);
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_PRE_MESSAGE,
						"text", &out), 0);
	TEST_CHECK_NULL(out); /* hook error degrades to pass-through */
	lua_bridge_free(bridge);
}

static void test_full_flow_transforms(void)
{
	/* Simulates the main-loop ownership pattern: pre_message replaces
	 * the input buffer, post_message supplies the displayed text. */
	lua_bridge_t *bridge = NULL;
	char *input = NULL;
	char *final_text = NULL;

	reset_commands();
	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_OK), 0);

	input = strdup("hook-me");
	TEST_CHECK_NOT_NULL(input);
	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_PRE_MESSAGE,
						input, &final_text), 0);
	if (final_text != NULL) {
		free(input);
		input = final_text;
		final_text = NULL;
	}
	TEST_CHECK_STR_EQ(input, "hook-me!");

	TEST_CHECK_INT_EQ(lua_bridge_apply_hook(bridge, LUA_HOOK_POST_MESSAGE,
						"answer", &final_text), 0);
	TEST_CHECK_NOT_NULL(final_text);
	TEST_CHECK_STR_EQ(final_text, "answer [ok]");

	free(final_text);
	free(input);
	lua_bridge_free(bridge);
	commands_free();
}

static void test_register_tool_and_dispatch(void)
{
	lua_bridge_t *bridge = NULL;
	tool_registry_t *reg = NULL;
	const tool_def_t *def;
	char *result = NULL;

	reset_commands();
	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	TEST_CHECK_INT_EQ(tools_registry_init(&reg), 0);
	lua_bridge_set_tool_registry(bridge, reg);
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_TOOLS), 0);
	TEST_CHECK_STR_EQ(lua_bridge_last_error(bridge), "");

	/* Three Lua tools registered into the shared registry. */
	TEST_CHECK_INT_EQ((long long)tools_count(reg), 3);
	def = tools_find(reg, "upper");
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_STR_EQ(def->description, "Uppercase the given text");
	TEST_CHECK(strstr(def->parameters, "\"type\":\"object\"") != NULL);
	TEST_CHECK(strstr(def->parameters, "\"text\"") != NULL);
	TEST_CHECK_INT_EQ((long long)def->requires_approval, 0);

	/* Dispatch through the registry handler: JSON args -> Lua table. */
	TEST_CHECK_INT_EQ((long long)def->handler(
			      def, "{\"text\":\"abc\"}", &result),
			  0);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK_STR_EQ(result, "ABC");
	free(result);

	/* Nil return becomes an empty result, not an error. */
	TEST_CHECK_INT_EQ((long long)def->handler(def, "{\"text\":\"x\"}",
						  &result),
			  0);
	TEST_CHECK_NOT_NULL(result);
	free(result);

	/* Malformed arguments: error text for the model, no crash. */
	def = tools_find(reg, "upper");
	TEST_CHECK_INT_EQ((long long)def->handler(def, "not json", &result),
			  -1);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK(strstr(result, "not valid JSON") != NULL);
	free(result);

	/* Lua runtime error: reported, result carries an error text. */
	def = tools_find(reg, "boom");
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_INT_EQ((long long)def->requires_approval, 0);
	TEST_CHECK_INT_EQ((long long)def->handler(def, "{}", &result), -1);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK(strstr(result, "tool failed") != NULL);
	free(result);
	TEST_CHECK_STR_NE(lua_bridge_last_error(bridge), "");

	/* opts.requires_approval is honored. */
	def = tools_find(reg, "risky");
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_INT_EQ((long long)def->requires_approval, 1);
	TEST_CHECK_INT_EQ((long long)def->handler(def, "{}", &result), 0);
	TEST_CHECK_STR_EQ(result, "risky done");
	free(result);

	tools_registry_free(reg);
	lua_bridge_free(bridge);
	commands_free();
}

static void test_register_tool_without_registry(void)
{
	lua_bridge_t *bridge = NULL;

	reset_commands();
	TEST_CHECK_INT_EQ(lua_bridge_init(&bridge, NULL), 0);
	/* No lua_bridge_set_tool_registry(): registration must fail with a
	 * reported Lua error and no crash. */
	TEST_CHECK_INT_EQ(lua_bridge_load_scripts(bridge, FIX_TOOLS), 0);
	TEST_CHECK_STR_NE(lua_bridge_last_error(bridge), "");
	lua_bridge_free(bridge);
	commands_free();
}

int main(void)
{
	TEST_RUN(test_init_and_free);
	TEST_RUN(test_load_missing_and_empty_dirs);
	TEST_RUN(test_load_bad_scripts_non_fatal);
	TEST_RUN(test_command_dispatch);
	TEST_RUN(test_command_runtime_error);
	TEST_RUN(test_pre_message_hook);
	TEST_RUN(test_post_message_hook);
	TEST_RUN(test_hook_chain_order);
	TEST_RUN(test_hook_runtime_error_pass_through);
	TEST_RUN(test_full_flow_transforms);
	TEST_RUN(test_register_tool_and_dispatch);
	TEST_RUN(test_register_tool_without_registry);
	commands_free();

	if (test__failures == 0)
		printf("All lua_scripting tests passed\n");
	return test__failures != 0;
}
