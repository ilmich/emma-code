/* POSIX symbols not declared by strict ISO C99 headers on this toolchain
 * are declared explicitly here instead of enabling feature-test macros
 * (see AGENTS.md portability rules). */
extern char *strdup(const char *s);

#include "theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Foreground SGR base codes of the 16-color palette; the background code
 * of a color is its foreground code plus 10. */
static const struct color_entry {
    const char *name;
    int code;
} color_table[] = {
    { "black",         30 },
    { "red",           31 },
    { "green",         32 },
    { "yellow",        33 },
    { "blue",          34 },
    { "magenta",       35 },
    { "cyan",          36 },
    { "white",         37 },
    { "bright_black",  90 },
    { "bright_red",    91 },
    { "bright_green",  92 },
    { "bright_yellow", 93 },
    { "bright_blue",   94 },
    { "bright_magenta", 95 },
    { "bright_cyan",   96 },
    { "bright_white",  97 },
};

/* Looks a palette name up; returns 0 and stores the foreground SGR code
 * on success, -1 when the name is unknown. */
static int color_code_by_name(const char *name, int *out_code) {
    if (name == NULL || out_code == NULL) return -1;
    for (size_t i = 0; i < sizeof(color_table) / sizeof(color_table[0]);
         i++) {
        if (strcmp(color_table[i].name, name) == 0) {
            *out_code = color_table[i].code;
            return 0;
        }
    }
    return -1;
}

/* True when s is a non-empty string of decimal digits only. */
static int is_all_digits(const char *s) {
    if (s == NULL || *s == '\0') return 0;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9') return 0;
    }
    return 1;
}

/* Builds a malloc'd SGR sequence from a printf-style body holding one
 * integer conversion (e.g. "\033[%dm"). Returns NULL on truncation or
 * allocation failure. */
static char *sgr_seq(const char *body, int code) {
    char buf[32];
    char *dup;
    int n = snprintf(buf, sizeof(buf), body, code);
    if (n < 0 || (size_t)n >= sizeof(buf)) return NULL;
    dup = strdup(buf);
    return dup;
}

/* Parses a foreground color spec (palette name or 256-color number)
 * into a malloc'd SGR sequence, or NULL when invalid. */
static char *parse_fg(const char *spec) {
    int code = 0;

    if (spec == NULL) return NULL;
    if (color_code_by_name(spec, &code) == 0) {
        return sgr_seq("\033[%dm", code);
    }
    if (is_all_digits(spec)) {
        long n = strtol(spec, NULL, 10);
        if (n < 0 || n > 255) return NULL;
        return sgr_seq("\033[38;5;%dm", (int)n);
    }
    return NULL;
}

/* Parses a background color spec (palette name, "on:<n>" 256-color
 * number) into a malloc'd SGR sequence, or NULL when invalid. */
static char *parse_bg(const char *spec) {
    int code = 0;

    if (spec == NULL) return NULL;
    if (strncmp(spec, "on:", 3) == 0) {
        const char *rest = spec + 3;
        if (is_all_digits(rest)) {
            long n = strtol(rest, NULL, 10);
            if (n < 0 || n > 255) return NULL;
            return sgr_seq("\033[48;5;%dm", (int)n);
        }
        return NULL; /* "on:" only accepts a numeric 256-color code */
    }
    if (color_code_by_name(spec, &code) == 0) {
        return sgr_seq("\033[%dm", code + 10);
    }
    return NULL;
}

/* Parses a reasoning style spec into a malloc'd SGR sequence, or NULL
 * when invalid. */
static char *parse_style(const char *spec) {
    if (spec == NULL) return NULL;
    if (strcmp(spec, "italic") == 0) {
        return strdup("\033[3m");
    }
    if (strcmp(spec, "dim") == 0) {
        return strdup("\033[2m");
    }
    if (strcmp(spec, "italic_dim") == 0 || strcmp(spec, "dim_italic") == 0) {
        return strdup("\033[2;3m");
    }
    return NULL;
}

int theme_default(theme_t *theme) {
    if (theme == NULL) return -1;

    theme->user_bg = NULL;
    theme->reasoning_on = NULL;
    theme->status = NULL;
    theme->input = NULL;
    theme->tool = NULL;
    theme->tool_output = NULL;
    theme->tool_error = NULL;

    theme->user_bg = strdup("\033[44m");     /* blue background */
    theme->reasoning_on = strdup("\033[2;3m"); /* dim + italic */
    theme->status = strdup("\033[1;33m");    /* bold yellow */
    theme->input = strdup("\033[1;32m");     /* bold green */
    theme->tool = strdup("\033[1;35m");      /* bold magenta */
    theme->tool_output = strdup("\033[90m"); /* bright black */
    theme->tool_error = strdup("\033[1;31m"); /* bold red */

    if (theme->user_bg == NULL || theme->reasoning_on == NULL ||
        theme->status == NULL || theme->input == NULL ||
        theme->tool == NULL || theme->tool_output == NULL ||
        theme->tool_error == NULL) {
        theme_free(theme);
        return -1;
    }
    return 0;
}

/* Replaces one theme field with a freshly parsed value, keeping the
 * old one (and warning) when the spec is invalid. Returns 0 when the
 * replacement succeeded, -1 on allocation failure. */
static int theme_replace(char **field, char *parsed, const char *spec) {
    if (parsed == NULL) {
        fprintf(stderr,
                "theme: invalid value '%s', keeping the default\n",
                spec != NULL ? spec : "");
        return 0;
    }
    free(*field);
    *field = parsed;
    return 0;
}

int theme_load(theme_t *theme, const toml_table_t *doc) {
    toml_type_t type;
    const void *value;
    const toml_table_t *section;
    const char *s = NULL;

    if (theme == NULL) return -1;
    if (doc == NULL) return 0; /* nothing to load: keep current values */

    value = toml_get(doc, "theme", &type);
    if (value == NULL || type != TOML_TABLE) {
        return 0; /* no [theme] section: keep current values */
    }
    section = (const toml_table_t *)value;

    if (toml_get_string(section, "user_bg", &s) == 0) {
        if (theme_replace(&theme->user_bg, parse_bg(s), s) != 0)
            return -1;
    }
    if (toml_get_string(section, "reasoning", &s) == 0) {
        if (theme_replace(&theme->reasoning_on, parse_style(s), s) != 0)
            return -1;
    }
    if (toml_get_string(section, "status", &s) == 0) {
        if (theme_replace(&theme->status, parse_fg(s), s) != 0)
            return -1;
    }
    if (toml_get_string(section, "input", &s) == 0) {
        if (theme_replace(&theme->input, parse_fg(s), s) != 0)
            return -1;
    }
    if (toml_get_string(section, "tool", &s) == 0) {
        if (theme_replace(&theme->tool, parse_fg(s), s) != 0)
            return -1;
    }
    if (toml_get_string(section, "tool_output", &s) == 0) {
        if (theme_replace(&theme->tool_output, parse_fg(s), s) != 0)
            return -1;
    }
    if (toml_get_string(section, "tool_error", &s) == 0) {
        if (theme_replace(&theme->tool_error, parse_fg(s), s) != 0)
            return -1;
    }
    return 0;
}

int theme_copy(theme_t *dst, const theme_t *src) {
    char *user_bg = NULL;
    char *reasoning_on = NULL;
    char *status = NULL;
    char *input = NULL;
    char *tool = NULL;
    char *tool_output = NULL;
    char *tool_error = NULL;
    int rc = -1;

    if (dst == NULL || src == NULL) return -1;

    if (src->user_bg != NULL) {
        user_bg = strdup(src->user_bg);
        if (user_bg == NULL) goto cleanup;
    }
    if (src->reasoning_on != NULL) {
        reasoning_on = strdup(src->reasoning_on);
        if (reasoning_on == NULL) goto cleanup;
    }
    if (src->status != NULL) {
        status = strdup(src->status);
        if (status == NULL) goto cleanup;
    }
    if (src->input != NULL) {
        input = strdup(src->input);
        if (input == NULL) goto cleanup;
    }
    if (src->tool != NULL) {
        tool = strdup(src->tool);
        if (tool == NULL) goto cleanup;
    }
    if (src->tool_output != NULL) {
        tool_output = strdup(src->tool_output);
        if (tool_output == NULL) goto cleanup;
    }
    if (src->tool_error != NULL) {
        tool_error = strdup(src->tool_error);
        if (tool_error == NULL) goto cleanup;
    }

    free(dst->user_bg);
    free(dst->reasoning_on);
    free(dst->status);
    free(dst->input);
    free(dst->tool);
    free(dst->tool_output);
    free(dst->tool_error);
    dst->user_bg = user_bg;
    dst->reasoning_on = reasoning_on;
    dst->status = status;
    dst->input = input;
    dst->tool = tool;
    dst->tool_output = tool_output;
    dst->tool_error = tool_error;
    user_bg = NULL;
    reasoning_on = NULL;
    status = NULL;
    input = NULL;
    tool = NULL;
    tool_output = NULL;
    tool_error = NULL;
    rc = 0;

cleanup:
    free(user_bg);
    free(reasoning_on);
    free(status);
    free(input);
    free(tool);
    free(tool_output);
    free(tool_error);
    return rc;
}

void theme_free(theme_t *theme) {
    if (theme == NULL) return;
    free(theme->user_bg);
    free(theme->reasoning_on);
    free(theme->status);
    free(theme->input);
    free(theme->tool);
    free(theme->tool_output);
    free(theme->tool_error);
    theme->user_bg = NULL;
    theme->reasoning_on = NULL;
    theme->status = NULL;
    theme->input = NULL;
    theme->tool = NULL;
    theme->tool_output = NULL;
    theme->tool_error = NULL;
}
