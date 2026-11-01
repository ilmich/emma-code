/**
 * @file main.c
 * @brief emma-code entry point: config loading, TUI setup, chat loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "commands.h"
#include "http_client.h"
#include "lua_scripting.h"
#include "openai_client.h"
#include "session_context.h"
#include "toml.h"
#include "tui.h"

#define MAX_HISTORY 10

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	if (d != NULL)
		memcpy(d, s, n);
	return d;
}

static void config_free_strings(openai_config_t *config)
{
	free(config->api_key);
	free(config->base_url);
	free(config->model_name);
	config->api_key = NULL;
	config->base_url = NULL;
	config->model_name = NULL;
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
 * (NULL when absent), owned by the caller.
 *
 * Returns 0 on success. On any failure -1 is returned, a diagnostic is
 * printed to stderr and all allocated strings are freed.
 */
static int load_config(openai_config_t *config, char **scripts_dir)
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
	*scripts_dir = NULL;

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
		goto out;
	}
	config->base_url = xstrdup(s);
	if (config->base_url == NULL) {
		fprintf(stderr, "emma: out of memory\n");
		goto out;
	}

	if (toml_get_string(doc, "api_key", &s) == 0) {
		config->api_key = xstrdup(s);
		if (config->api_key == NULL) {
			fprintf(stderr, "emma: out of memory\n");
			goto out;
		}
	}

	if (toml_get_string(doc, "model", &s) == 0) {
		config->model_name = xstrdup(s);
	} else {
		config->model_name = xstrdup("gpt-3.5-turbo");
	}
	if (config->model_name == NULL) {
		fprintf(stderr, "emma: out of memory\n");
		goto out;
	}

	if (toml_get_double(doc, "temperature", &d) == 0)
		config->temperature = d;

	if (toml_get_string(doc, "scripts_dir", &s) == 0) {
		*scripts_dir = xstrdup(s);
		if (*scripts_dir == NULL) {
			fprintf(stderr, "emma: out of memory\n");
			goto out;
		}
	}

	rc = 0;
out:
	toml_free(doc);
	if (rc != 0) {
		config_free_strings(config);
		free(*scripts_dir);
		*scripts_dir = NULL;
	}
	return rc;
}

int main(void) {
    if (http_client_init() != 0) {
        return 1;
    }

    openai_config_t config;
    char *scripts_dir = NULL;
    if (load_config(&config, &scripts_dir) != 0) {
        http_client_cleanup();
        return 1;
    }

    session_context_t context = {0};
    if (session_context_init(&context, MAX_HISTORY) != 0) {
        config_free_strings(&config);
        http_client_cleanup();
        return 1;
    }

    tui_t *tui = NULL;
    if (tui_init(&tui) != 0) {
        session_context_free(&context);
        config_free_strings(&config);
        http_client_cleanup();
        return 1;
    }

    if (commands_init() != 0) {
        fprintf(stderr, "emma: failed to initialize command registry\n");
        session_context_free(&context);
        config_free_strings(&config);
        free(scripts_dir);
        http_client_cleanup();
        return 1;
    }

    lua_bridge_t *bridge = NULL;
    if (lua_bridge_init(&bridge, tui) != 0) {
        fprintf(stderr, "emma: failed to initialize Lua scripting engine\n");
        session_context_free(&context);
        config_free_strings(&config);
        free(scripts_dir);
        http_client_cleanup();
        commands_free();
        return 1;
    }

    if (scripts_dir == NULL) {
        char default_scripts[512];
        if (scripts_dir_default(default_scripts, sizeof default_scripts) == 0)
            scripts_dir = xstrdup(default_scripts);
    }
    if (scripts_dir == NULL ||
        lua_bridge_load_scripts(bridge, scripts_dir) != 0) {
        fprintf(stderr, "emma: failed to load Lua scripts\n");
    }
    free(scripts_dir);
    scripts_dir = NULL;

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

        if (session_context_add_message(&context, "user", input) != 0) {
            free(input);
            break;
        }
        tui_append_message(tui, "user", input);
        free(input);

        tui_set_status(tui, "Thinking...");

        openai_response_t response = {0};
        int result = openai_chat_completion(&config, context.messages, context.count, &response);

        tui_set_status(tui, NULL);

        if (result == 0) {
            /* post_message hook: fires when the LLM completes its
             * response, before display; the returned text (if any)
             * replaces the original in the UI and in the history. */
            char *final_text = NULL;
            if (lua_bridge_apply_hook(bridge, LUA_HOOK_POST_MESSAGE,
                                      response.content,
                                      &final_text) != 0)
                final_text = NULL;
            const char *text = final_text != NULL ? final_text
                                                  : response.content;
            tui_append_message(tui, "assistant", text);
            session_context_add_message(&context, "assistant", text);
            free(final_text);
            openai_response_free(&response);
        } else {
            tui_append_message(tui, "assistant", "[Error: failed to get response]");
        }
    }

    tui_free(tui);
    lua_bridge_free(bridge);
    session_context_free(&context);
    config_free_strings(&config);
    http_client_cleanup();
    commands_free();
    return 0;
}
