/**
 * @file main.c
 * @brief emma-code entry point: config loading, TUI setup, command
 *        dispatch and wiring of the agentic chat loop (src/agent_loop.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_loop.h"
#include "theme.h"
#include "util.h"
#include "commands.h"
#include "http_client.h"
#include "lua_scripting.h"
#include "openai_client.h"
#include "session_context.h"
#include "system_prompt.h"
#include "toml.h"
#include "tools.h"
#include "tui.h"

#define DEFAULT_MAX_HISTORY 100
#define DEFAULT_MAX_TOOL_ROUNDS 10

/** Context handed to the tool dispatcher (all pointers borrowed). */
typedef struct {
	tool_registry_t *tools;
	tui_t *tui;
} tool_dispatch_ctx_t;

static void config_free_strings(openai_config_t *config)
{
	free(config->api_key);
	free(config->base_url);
	free(config->model_name);
	free(config->tools_json);
	config->api_key = NULL;
	config->base_url = NULL;
	config->model_name = NULL;
	config->tools_json = NULL;
}

/**
 * Builds the config file path: $XDG_CONFIG_HOME/emma/config.toml, or
 * ~/.config/emma/config.toml when XDG_CONFIG_HOME is unset.
 * Returns 0 on success, -1 when the path does not fit the buffer.
 */
static int config_path(char *buf, size_t buflen)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	int n;

	if (xdg != NULL && *xdg != '\0')
		n = snprintf(buf, buflen, "%s/emma/config.toml", xdg);
	else if (home != NULL && *home != '\0')
		n = snprintf(buf, buflen, "%s/.config/emma/config.toml", home);
	else
		return -1;
	if (n < 0 || (size_t)n >= buflen)
		return -1;
	return 0;
}

/**
 * Builds the default scripts directory: $XDG_CONFIG_HOME/emma/scripts,
 * or ~/.config/emma/scripts when XDG_CONFIG_HOME is unset.
 * Returns 0 on success, -1 when the path does not fit the buffer.
 */
static int scripts_dir_default(char *buf, size_t buflen)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	int n;

	if (xdg != NULL && *xdg != '\0')
		n = snprintf(buf, buflen, "%s/emma/scripts", xdg);
	else if (home != NULL && *home != '\0')
		n = snprintf(buf, buflen, "%s/.config/emma/scripts", home);
	else
		return -1;
	if (n < 0 || (size_t)n >= buflen)
		return -1;
	return 0;
}

/**
 * Loads openai_config_t from the TOML config file. base_url is mandatory;
 * api_key, model_name and temperature fall back to hardcoded defaults when
 * absent from the file. No environment variable contributes to the
 * configuration values themselves.
 *
 * On success *scripts_dir receives the optional scripts_dir string value
 * (NULL when absent), owned by the caller; *max_history and
 * *max_tool_rounds receive the optional numeric limits (defaults when
 * absent or invalid).
 *
 * Returns 0 on success. On any failure -1 is returned, a diagnostic is
 * printed to stderr and all allocated strings are freed.
 */
static int load_config(openai_config_t *config, char **scripts_dir,
		       size_t *max_history, int *max_tool_rounds,
		       theme_t *theme)
{
	char path[512];
	char errbuf[256];
	toml_table_t *doc = NULL;
	const char *s = NULL;
	double d = 0.0;
	int rc = -1;

	config->api_key = NULL;
	config->base_url = NULL;
	config->model_name = NULL;
	config->temperature = 0.7;
	config->stream = 0;
	config->tools_json = NULL;
	*scripts_dir = NULL;
	*max_history = DEFAULT_MAX_HISTORY;
	*max_tool_rounds = DEFAULT_MAX_TOOL_ROUNDS;

	if (config_path(path, sizeof path) != 0) {
		fprintf(stderr, "emma: config path too long\n");
		return -1;
	}

	if (toml_parse_file(path, &doc, errbuf, sizeof errbuf) != 0) {
		fprintf(stderr, "emma: %s\n", errbuf);
		return -1;
	}

	if (toml_get_string(doc, "base_url", &s) != 0) {
		fprintf(stderr, "emma: missing base_url in %s\n", path);
		goto cleanup;
	}
	config->base_url = util_strdup(s);
	if (config->base_url == NULL) {
		fprintf(stderr, "emma: out of memory\n");
		goto cleanup;
	}

	if (toml_get_string(doc, "api_key", &s) == 0) {
		config->api_key = util_strdup(s);
		if (config->api_key == NULL) {
			fprintf(stderr, "emma: out of memory\n");
			goto cleanup;
		}
	}

	if (toml_get_string(doc, "model", &s) == 0) {
		config->model_name = util_strdup(s);
	} else {
		config->model_name = util_strdup("gpt-3.5-turbo");
	}
	if (config->model_name == NULL) {
		fprintf(stderr, "emma: out of memory\n");
		goto cleanup;
	}

	if (toml_get_double(doc, "temperature", &d) == 0)
		config->temperature = d;

	/* Optional limits for the agentic tool loop and the history size. */
	{
		long long rounds = 0;
		long long hist = 0;

		if (toml_get_int(doc, "max_tool_rounds", &rounds) == 0 &&
		    rounds > 0)
			*max_tool_rounds = (int)rounds;
		if (toml_get_int(doc, "max_history", &hist) == 0 && hist > 0)
			*max_history = (size_t)hist;
	}

	/* Optional boolean: stream the completion via SSE. Defaults to 0
	 * (non-streaming) when absent or not a boolean. */
	if (toml_get_bool(doc, "stream", &config->stream) != 0)
		config->stream = 0;

	if (toml_get_string(doc, "scripts_dir", &s) == 0) {
		*scripts_dir = util_strdup(s);
		if (*scripts_dir == NULL) {
			fprintf(stderr, "emma: out of memory\n");
			goto cleanup;
		}
	}

	/* Optional [theme] section: overrides the built-in default colors.
	 * Invalid values keep the defaults (theme_load warns) and never
	 * fail the startup. */
	if (theme_default(theme) != 0) {
		fprintf(stderr, "emma: out of memory\n");
		goto cleanup;
	}
	if (theme_load(theme, doc) != 0) {
		fprintf(stderr, "emma: failed to load [theme] section\n");
		goto cleanup;
	}

	rc = 0;
cleanup:
	toml_free(doc);
	if (rc != 0) {
		config_free_strings(config);
		theme_free(theme);
		free(*scripts_dir);
		*scripts_dir = NULL;
	}
	return rc;
}

/**
 * Builds a short single-line preview of the tool arguments for the TUI.
 * Truncation-safe: the buffer is always NUL-terminated.
 */
static void tool_args_preview(char *buf, size_t buflen, const char *args)
{
	int n;

	if (buflen == 0)
		return;
	if (args == NULL)
		n = snprintf(buf, buflen, "(no arguments)");
	else
		n = snprintf(buf, buflen, "%s", args);
	if (n < 0 || (size_t)n >= buflen)
		snprintf(buf, buflen, "%.*s...", (int)(buflen - 4),
			 args != NULL ? args : "");
	/* Flatten newlines so the preview stays on one status line. */
	for (char *p = buf; *p != '\0'; ++p) {
		if (*p == '\n' || *p == '\r')
			*p = ' ';
	}
}

/**
 * Tool dispatcher wired into the agent loop: looks the tool up in the
 * registry, asks the user for confirmation when the tool is marked as
 * requiring approval, executes it and hands the text result back to the
 * model. Execution failures are never fatal: the error text is sent to
 * the model as the tool result. Returns nonzero only on malloc failure.
 */
static int dispatch_tool(const char *name, const char *arguments_json,
			 char **result, void *user_data)
{
	tool_dispatch_ctx_t *ctx = user_data;
	const tool_def_t *def;
	char preview[160];
	char line[224];
	int n;

	if (result == NULL)
		return 1;
	*result = NULL;
	if (ctx == NULL || ctx->tools == NULL || name == NULL) {
		*result = util_strdup("[tool error: no tool registry]");
		return *result == NULL;
	}

	def = tools_find(ctx->tools, name);
	if (def == NULL) {
		n = snprintf(line, sizeof line, "[tool error: unknown tool '%s']",
			     name);
		if (n < 0 || (size_t)n >= sizeof line)
			*result = util_strdup("[tool error: unknown tool]");
		else
			*result = util_strdup(line);
		return *result == NULL;
	}

	tool_args_preview(preview, sizeof preview, arguments_json);
	n = snprintf(line, sizeof line, "[tool] %s: %s", name, preview);
	if (n >= 0 && (size_t)n < sizeof line && ctx->tui != NULL)
		tui_append_message(ctx->tui, "system", line);

	if (def->requires_approval) {
		char prompt[192];

		n = snprintf(prompt, sizeof prompt, "Allow %s? (%s)", name,
			     preview);
		if (n < 0 || (size_t)n >= sizeof prompt)
			n = snprintf(prompt, sizeof prompt, "Allow %s?", name);
		if (tui_confirm(ctx->tui, prompt) == 0) {
			*result = util_strdup("[tool] denied by user");
			return *result == NULL;
		}
	}

	if (ctx->tui != NULL)
		tui_set_status(ctx->tui, NULL);
	def->handler(def, arguments_json != NULL ? arguments_json : "",
		     result);
	if (*result == NULL)
		*result = util_strdup("[tool error: out of memory]");
	return *result == NULL;
}

/* Reset before every message: clears the status line on the first
 * streamed delta so it does not linger over the live reply. */
static int g_status_cleared;

/** Forward each streamed delta to the TUI (streaming reply rendering). */
static void on_delta(const char *text, int is_reasoning, void *user_data)
{
	tui_t *tui = user_data;

	if (tui == NULL || text == NULL || *text == '\0')
		return;
	if (!g_status_cleared) {
		g_status_cleared = 1;
		tui_set_status(tui, NULL);
	}
	if (is_reasoning)
		tui_stream_reasoning_append(tui, text);
	else
		tui_stream_append(tui, text);
}

int main(void) {
    openai_config_t config;
    char *scripts_dir = NULL;
    char *prompt = NULL;
    session_context_t context = {0};
    tui_t *tui = NULL;
    lua_bridge_t *bridge = NULL;
    tool_registry_t *tools = NULL;
    tool_dispatch_ctx_t tool_ctx = {0};
    size_t max_history = DEFAULT_MAX_HISTORY;
    int max_tool_rounds = DEFAULT_MAX_TOOL_ROUNDS;
    theme_t theme = {NULL, NULL, NULL, NULL};
    int commands_ready = 0;
    int http_ready = 0;
    int status = 1;

    if (http_client_init() != 0) {
        return 1;
    }
    http_ready = 1;

    if (load_config(&config, &scripts_dir, &max_history,
                    &max_tool_rounds, &theme) != 0)
        goto cleanup;

    if (session_context_init(&context, max_history) != 0)
        goto cleanup;

    /* Seed the session with the built-in system prompt (plus the
     * environment info block) as the very first message: the truncation
     * policy never evicts role "system", so the prompt stays a stable,
     * cache-friendly prefix of every request. */
    prompt = system_prompt_full();
    if (prompt == NULL ||
        session_context_add_message(&context, "system", prompt) != 0) {
        fprintf(stderr, "emma: out of memory\n");
        goto cleanup;
    }

    if (tui_init(&tui, &theme) != 0)
        goto cleanup;
    /* The TUI owns a copy of the theme; release ours right away. */
    theme_free(&theme);

    if (commands_init() != 0) {
        fprintf(stderr, "emma: failed to initialize command registry\n");
        goto cleanup;
    }
    commands_ready = 1;

    if (tools_registry_init(&tools) != 0) {
        fprintf(stderr, "emma: failed to initialize tool registry\n");
        goto cleanup;
    }
    if (tools_register_builtins(tools) != 0) {
        fprintf(stderr, "emma: failed to register built-in tools\n");
        goto cleanup;
    }
    config.tools_json = tools_build_json(tools);
    if (config.tools_json == NULL) {
        fprintf(stderr, "emma: failed to serialize tool definitions\n");
        goto cleanup;
    }

    if (lua_bridge_init(&bridge, tui) != 0) {
        fprintf(stderr, "emma: failed to initialize Lua scripting engine\n");
        goto cleanup;
    }
    lua_bridge_set_tool_registry(bridge, tools);

    if (scripts_dir == NULL) {
        char default_scripts[512];
        if (scripts_dir_default(default_scripts, sizeof default_scripts) == 0)
            scripts_dir = util_strdup(default_scripts);
    }
    if (scripts_dir == NULL ||
        lua_bridge_load_scripts(bridge, scripts_dir) != 0) {
        fprintf(stderr, "emma: failed to load Lua scripts\n");
    }
    free(scripts_dir);
    scripts_dir = NULL;

    /* Agentic loop context: all pointers are borrowed by agent_loop.
     * The TUI stays here: streamed deltas are forwarded by on_delta and
     * tool executions go through dispatch_tool. */
    agent_loop_t loop = {0};
    loop.config = &config;
    loop.session = &context;
    loop.on_delta = on_delta;
    loop.delta_user_data = tui;
    loop.on_tool_call = dispatch_tool;
    loop.tool_user_data = &tool_ctx;
    loop.max_tool_rounds = max_tool_rounds;
    tool_ctx.tools = tools;
    tool_ctx.tui = tui;

    tui_render(tui);

    while (1) {
        char *input = tui_read_input(tui);
        if (!input) break;

        char feedback[256];
        cmd_result_t cres = commands_dispatch(input, feedback, sizeof feedback);
        if (cres == CMD_EXIT) {
            free(input);
            break;
        }
        if (cres == CMD_HANDLED) {
            tui_append_message(tui, "system", feedback);
            free(input);
            continue;
        }

        /* pre_message hook: fires when the user presses enter, before
         * the message is stored or displayed; the returned text (if any)
         * replaces the original everywhere. */
        char *hooked = NULL;
        if (lua_bridge_apply_hook(bridge, LUA_HOOK_PRE_MESSAGE, input,
                                  &hooked) == 0 && hooked != NULL) {
            free(input);
            input = hooked;
        }

        /* Show the user message (with any pre_message replacement) and
         * turn on the status line; agent_loop performs no rendering. */
        tui_append_message(tui, "user", input);
        tui_set_status(tui, "Thinking...");
        g_status_cleared = 0;

        /* Send the message and get the raw assistant reply; command
         * dispatch, hooks and all rendering stay here, out of agent_loop. */
        char *reply = NULL;
        char *reasoning = NULL;
        agent_msg_result_t res = agent_loop_handle_message(&loop, input, &reply,
                                                           &reasoning);
        tui_set_status(tui, NULL);
        if (res == AGENT_MSG_EXIT)
            break;
        if (res == AGENT_MSG_ERROR) {
            tui_append_message(tui, "assistant",
                               "[Error: failed to get response]");
            continue;
        }

        /* post_message hook: fires when the LLM completes its response,
         * before display; the returned text (if any) replaces the
         * original in the UI and in the history. */
        char *final_text = NULL;
        if (lua_bridge_apply_hook(bridge, LUA_HOOK_POST_MESSAGE, reply,
                                  &final_text) != 0)
            final_text = NULL;
        const char *text = final_text != NULL ? final_text : reply;
        /* Replace the streamed assistant message with the final reply
         * instead of appending a second copy of the same message. The
         * reasoning is display-only: it is shown above the reply but
         * never stored in the session history. */
        tui_replace_assistant_message(tui, text, reasoning);
        if (session_context_add_message(&context, "assistant", text) != 0)
            tui_append_message(tui, "system",
                               "[Warning: failed to store assistant "
                               "message in history]");
        free(final_text);
        free(reply);
        free(reasoning);
    }

    status = 0;

cleanup:
    free(scripts_dir);
    theme_free(&theme);
    if (tui != NULL) {
        tui_free(tui);
    }
    lua_bridge_free(bridge);
    tools_registry_free(tools);
    if (commands_ready) {
        commands_free();
    }
    free(prompt);
    session_context_free(&context);
    config_free_strings(&config);
    if (http_ready) {
        http_client_cleanup();
    }
    return status;
}
