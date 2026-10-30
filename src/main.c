/**
 * @file main.c
 * @brief emma-code entry point: config loading, TUI setup, chat loop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_client.h"
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
 * Loads openai_config_t from the TOML config file. base_url is mandatory;
 * api_key, model_name and temperature fall back to hardcoded defaults when
 * absent from the file. No environment variable contributes to the
 * configuration values themselves.
 *
 * Returns 0 on success. On any failure -1 is returned, a diagnostic is
 * printed to stderr and all allocated strings are freed.
 */
static int load_config(openai_config_t *config)
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

	rc = 0;
out:
	toml_free(doc);
	if (rc != 0)
		config_free_strings(config);
	return rc;
}

int main(void) {
    if (http_client_init() != 0) {
        return 1;
    }

    openai_config_t config;
    if (load_config(&config) != 0) {
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

    tui_render(tui);

    while (1) {
        char *input = tui_read_input(tui);
        if (!input) break;

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
            tui_append_message(tui, "assistant", response.content);
            session_context_add_message(&context, "assistant", response.content);
            openai_response_free(&response);
        } else {
            tui_append_message(tui, "assistant", "[Error: failed to get response]");
        }
    }

    tui_free(tui);
    session_context_free(&context);
    config_free_strings(&config);
    http_client_cleanup();
    return 0;
}
