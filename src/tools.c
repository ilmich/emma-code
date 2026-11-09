/**
 * @file tools.c
 * @brief Runtime tool registry and built-in tools (read_file, write_file,
 *        bash).
 *
 * POSIX-only symbols (popen/pclose) are declared explicitly instead of
 * enabling feature-test macros (see AGENTS.md portability rules).
 */

#include "tools.h"
#include "util.h"

#include "cjson/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern FILE *popen(const char *command, const char *mode);
extern int pclose(FILE *stream);

struct tool_registry {
    tool_def_t *defs;
    size_t count;
    size_t cap;
};

/* ------------------------------------------------------------------ */
/* Registry                                                            */
/* ------------------------------------------------------------------ */

int tools_registry_init(tool_registry_t **reg)
{
    tool_registry_t *r;

    if (reg == NULL)
        return -1;
    *reg = NULL;
    r = calloc(1, sizeof(*r));
    if (r == NULL)
        return -1;
    *reg = r;
    return 0;
}

void tools_registry_free(tool_registry_t *reg)
{
    if (reg == NULL)
        return;
    for (size_t i = 0; i < reg->count; ++i) {
        free(reg->defs[i].name);
        free(reg->defs[i].description);
        free(reg->defs[i].parameters);
    }
    free(reg->defs);
    free(reg);
}

size_t tools_count(const tool_registry_t *reg)
{
    return reg != NULL ? reg->count : 0;
}

const tool_def_t *tools_at(const tool_registry_t *reg, size_t i)
{
    if (reg == NULL || i >= reg->count)
        return NULL;
    return &reg->defs[i];
}

const tool_def_t *tools_find(const tool_registry_t *reg, const char *name)
{
    if (reg == NULL || name == NULL)
        return NULL;
    for (size_t i = 0; i < reg->count; ++i) {
        if (reg->defs[i].name != NULL && strcmp(reg->defs[i].name, name) == 0)
            return &reg->defs[i];
    }
    return NULL;
}

int tools_register(tool_registry_t *reg, const char *name,
                   const char *description, const char *parameters_schema,
                   int requires_approval, tool_handler_fn handler,
                   tool_title_fn title, void *userdata)
{
    tool_def_t *def;
    char *name_copy = NULL;
    char *desc_copy = NULL;
    char *params_copy = NULL;
    int rc = -1;

    if (reg == NULL || name == NULL || *name == '\0' ||
        strlen(name) >= TOOLS_NAME_MAX || description == NULL ||
        parameters_schema == NULL || handler == NULL)
        return -1;
    if (tools_find(reg, name) != NULL)
        return -1; /* duplicate name */

    name_copy = util_strdup(name);
    desc_copy = util_strdup(description);
    params_copy = util_strdup(parameters_schema);
    if (name_copy == NULL || desc_copy == NULL || params_copy == NULL)
        goto cleanup;

    if (reg->count >= reg->cap) {
        size_t new_cap = reg->cap == 0 ? 8 : reg->cap * 2;
        tool_def_t *grown = realloc(reg->defs, new_cap * sizeof(*grown));

        if (grown == NULL)
            goto cleanup;
        reg->defs = grown;
        reg->cap = new_cap;
    }

    def = &reg->defs[reg->count];
    def->name = name_copy;
    def->description = desc_copy;
    def->parameters = params_copy;
    def->requires_approval = requires_approval != 0;
    def->handler = handler;
    def->title = title;
    def->userdata = userdata;
    name_copy = NULL; /* ownership transferred to the registry */
    desc_copy = NULL;
    params_copy = NULL;
    reg->count++;
    rc = 0;

cleanup:
    free(name_copy);
    free(desc_copy);
    free(params_copy);
    return rc;
}

char *tools_build_json(const tool_registry_t *reg)
{
    cJSON *array;
    char *json;

    if (reg == NULL || reg->count == 0)
        return NULL;

    array = cJSON_CreateArray();
    if (array == NULL)
        return NULL;

    for (size_t i = 0; i < reg->count; ++i) {
        cJSON *entry = cJSON_CreateObject();
        cJSON *fn = NULL;
        cJSON *params = NULL;
        int ok = entry != NULL && (fn = cJSON_CreateObject()) != NULL;

        if (ok) {
            /* The parameters field is a pre-serialized JSON schema:
             * parse it so it nests as an object, not as a string. */
            params = cJSON_Parse(reg->defs[i].parameters);
            ok = params != NULL &&
                 cJSON_AddStringToObject(entry, "type", "function") != NULL &&
                 cJSON_AddItemToObject(entry, "function", fn) != 0 &&
                 cJSON_AddStringToObject(fn, "name", reg->defs[i].name) != NULL &&
                 cJSON_AddStringToObject(fn, "description",
                                         reg->defs[i].description) != NULL &&
                 cJSON_AddItemToObject(fn, "parameters", params) != 0;
        }
        if (!ok) {
            cJSON_Delete(entry); /* tolerates NULL */
            if (params != NULL && (entry == NULL ||
                cJSON_GetObjectItem(entry, "parameters") == NULL))
                cJSON_Delete(params); /* not attached */
            cJSON_Delete(array);
            return NULL;
        }
        if (cJSON_AddItemToArray(array, entry) == 0) {
            cJSON_Delete(entry);
            cJSON_Delete(array);
            return NULL;
        }
    }

    json = cJSON_PrintUnformatted(array);
    if (json != NULL) {
        char *copy = util_strdup(json);

        cJSON_free(json);
        json = copy;
    }
    cJSON_Delete(array);
    return json;
}

/* ------------------------------------------------------------------ */
/* Shared argument parsing                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Extracts a required string field from the tool arguments JSON.
 *
 * @param args_json Raw arguments string (may be NULL or malformed).
 * @param field Field name to extract.
 * @param errbuf Buffer receiving an error description on failure.
 * @param errbuf_len Size of errbuf.
 * @return malloc'ed copy of the field value owned by the caller, or NULL
 *         (errbuf describes the problem).
 */
static char *extract_string_arg(const char *args_json, const char *field,
                                char *errbuf, size_t errbuf_len)
{
    cJSON *root = NULL;
    cJSON *item;
    char *value = NULL;

    if (args_json == NULL || *args_json == '\0') {
        snprintf(errbuf, errbuf_len, "missing arguments");
        return NULL;
    }
    root = cJSON_Parse(args_json);
    if (root == NULL) {
        snprintf(errbuf, errbuf_len, "arguments are not valid JSON");
        return NULL;
    }
    item = cJSON_GetObjectItem(root, field);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        snprintf(errbuf, errbuf_len, "missing or non-string '%s' argument",
                 field);
        cJSON_Delete(root);
        return NULL;
    }
    value = util_strdup(item->valuestring);
    cJSON_Delete(root);
    if (value == NULL)
        snprintf(errbuf, errbuf_len, "out of memory");
    return value;
}

/**
 * @brief Reads a whole stream into a malloc'ed buffer, truncating at
 *        TOOLS_RESULT_MAX. The buffer is always NUL-terminated.
 *
 * @return malloc'ed buffer owned by the caller, or NULL on allocation
 *         failure or when the stream cannot be opened.
 */
static char *read_stream(FILE *fp, int *truncated, size_t *cap_out)
{
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);

    *truncated = 0;
    *cap_out = 0;
    if (buf == NULL)
        return NULL;
    buf[0] = '\0'; /* empty stream: always return a valid empty string */
    /* Reserve headroom for the trailing notices ([output truncated] /
     * [exit status: N]) so they always fit under TOOLS_RESULT_MAX. */
    while (fgets(buf + len, (int)(cap - len), fp) != NULL) {
        len += strlen(buf + len);
        if (cap - len < 2 + TOOLS_NOTICE_ROOM) {
            if (cap >= TOOLS_RESULT_MAX) {
                *truncated = 1;
                break;
            }
            size_t new_cap = cap * 2;
            char *grown = realloc(buf, new_cap);

            if (grown == NULL)
                break; /* keep what we have */
            buf = grown;
            cap = new_cap;
        }
    }
    if (len == 0 && *truncated == 0 && ferror(fp)) {
        free(buf);
        return NULL;
    }
    *cap_out = cap;
    return buf;
}

/** Appends a truncation notice when output was cut short. */
static char *finalize_result(char *buf, int truncated)
{
    size_t len;

    if (buf == NULL)
        return NULL;
    if (!truncated)
        return buf;
    len = strlen(buf);
    if (len + 32 <= TOOLS_RESULT_MAX)
        snprintf(buf + len, 32, "\n[output truncated]");
    return buf;
}

/* ------------------------------------------------------------------ */
/* Built-in tool handlers                                              */
/* ------------------------------------------------------------------ */

/** read_file: {"path"} -> file content. */
static int tool_read_file(const tool_def_t *tool, const char *args_json,
                          char **result)
{
    char errbuf[128];
    char *path = NULL;
    FILE *fp = NULL;
    char *buf = NULL;
    size_t cap = 0;
    int truncated = 0;
    int rc = -1;

    (void)tool;
    if (result == NULL)
        return -1;
    *result = NULL;
    path = extract_string_arg(args_json, "path", errbuf, sizeof errbuf);
    if (path == NULL) {
        *result = util_strdup(errbuf);
        return -1;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        snprintf(errbuf, sizeof errbuf, "cannot open '%s'", path);
        *result = util_strdup(errbuf);
        goto cleanup;
    }
    buf = read_stream(fp, &truncated, &cap);
    if (buf == NULL) {
        snprintf(errbuf, sizeof errbuf, "cannot read '%s'", path);
        *result = util_strdup(errbuf);
        goto cleanup;
    }
    *result = finalize_result(buf, truncated);
    buf = NULL; /* ownership transferred to *result */
    rc = 0;

cleanup:
    if (fp != NULL)
        fclose(fp);
    free(path);
    free(buf);
    return rc;
}

/** write_file: {"path", "content"} -> confirmation. */
static int tool_write_file(const tool_def_t *tool, const char *args_json,
                           char **result)
{
    char errbuf[128];
    char *path = NULL;
    char *content = NULL;
    FILE *fp = NULL;
    int rc = -1;

    (void)tool;
    if (result == NULL)
        return -1;
    *result = NULL;
    path = extract_string_arg(args_json, "path", errbuf, sizeof errbuf);
    if (path == NULL) {
        *result = util_strdup(errbuf);
        return -1;
    }
    content = extract_string_arg(args_json, "content", errbuf,
                                 sizeof errbuf);
    if (content == NULL) {
        *result = util_strdup(errbuf);
        goto cleanup;
    }
    fp = fopen(path, "wb");
    if (fp == NULL) {
        snprintf(errbuf, sizeof errbuf, "cannot open '%s' for writing",
                 path);
        *result = util_strdup(errbuf);
        goto cleanup;
    }
    if (fwrite(content, 1, strlen(content), fp) != strlen(content) ||
        fflush(fp) != 0) {
        snprintf(errbuf, sizeof errbuf, "cannot write '%s'", path);
        *result = util_strdup(errbuf);
        goto cleanup;
    }
    snprintf(errbuf, sizeof errbuf, "wrote %zu bytes to %s",
             strlen(content), path);
    *result = util_strdup(errbuf);
    if (*result == NULL)
        goto cleanup;
    rc = 0;

cleanup:
    if (fp != NULL)
        fclose(fp);
    free(path);
    free(content);
    return rc;
}

/** bash: {"command"} -> combined stdout+stderr and exit status. */
static int tool_bash(const tool_def_t *tool, const char *args_json,
                     char **result)
{
    char errbuf[128];
    char *command = NULL;
    char *full = NULL;
    FILE *fp = NULL;
    char *buf = NULL;
    size_t cap = 0;
    int truncated = 0;
    int rc = -1;

    (void)tool;
    if (result == NULL)
        return -1;
    *result = NULL;
    command = extract_string_arg(args_json, "command", errbuf,
                                 sizeof errbuf);
    if (command == NULL) {
        *result = util_strdup(errbuf);
        return -1;
    }
    if (*command == '\0') {
        *result = util_strdup("empty command");
        goto cleanup;
    }

    /* popen already runs via /bin/sh -c: merge stderr into stdout so the
     * model sees diagnostics too. */
    full = malloc(strlen(command) + sizeof(" 2>&1"));
    if (full == NULL) {
        *result = util_strdup("out of memory");
        goto cleanup;
    }
    sprintf(full, "%s 2>&1", command);
    fp = popen(full, "r");
    if (fp == NULL) {
        *result = util_strdup("cannot run command");
        goto cleanup;
    }
    buf = read_stream(fp, &truncated, &cap);
    if (buf == NULL) {
        *result = util_strdup("cannot read command output");
        goto cleanup;
    }
    {
        int status = pclose(fp);
        int code;
        size_t len;

        fp = NULL;
        /* Decode the raw wait status: a signal death (e.g. SIGPIPE after
         * we stopped reading a truncated output) is reported as 128+sig. */
        if (WIFEXITED(status))
            code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            code = 128 + WTERMSIG(status);
        else
            code = status;
        /* When we truncated the output the command may have been killed
         * by SIGPIPE: that is not a tool failure. A genuine nonzero exit
         * is still handed to the model, but reported as an error. */
        rc = (truncated || code == 0) ? 0 : -1;
        len = strlen(buf);
        /* The append must fit both the allocated buffer and the
         * TOOLS_RESULT_MAX cap enforced by finalize_result(). */
        if (len + 32 <= cap && len + 32 <= TOOLS_RESULT_MAX) {
            snprintf(buf + len, 32, "\n[exit status: %d]", code);
        }
    }
    *result = finalize_result(buf, truncated);
    buf = NULL; /* ownership transferred to *result */

cleanup:
    if (fp != NULL)
        pclose(fp);
    free(full);
    free(command);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Built-in registration                                               */
/* ------------------------------------------------------------------ */

/* Builds the one-line block title for the built-in tools: the tool name
 * followed by its key argument (the command for bash, the file path for
 * read_file/write_file). Malformed arguments fall back to NULL (the
 * dispatcher then uses the generic format). */
static char *builtin_title(const tool_def_t *tool, const char *args_json)
{
    char errbuf[128];
    char *value = NULL;
    char *title = NULL;
    size_t len;
    int n;

    if (tool == NULL || tool->name == NULL)
        return NULL;
    if (strcmp(tool->name, "bash") == 0)
        value = extract_string_arg(args_json, "command", errbuf,
                                   sizeof errbuf);
    else
        value = extract_string_arg(args_json, "path", errbuf,
                                   sizeof errbuf);
    if (value == NULL)
        return NULL;

    len = strlen(tool->name) + 1 + strlen(value);
    title = malloc(len + 1);
    if (title == NULL) {
        free(value);
        return NULL;
    }
    n = snprintf(title, len + 1, "%s %s", tool->name, value);
    if (n < 0 || (size_t)n >= len + 1) {
        free(value);
        free(title);
        return NULL;
    }
    free(value);
    return title;
}

int tools_register_builtins(tool_registry_t *reg)
{
    static const char *read_schema =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"Path of the file to read\"}},\"required\":[\"path\"]}";
    static const char *write_schema =
        "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"Path of the file to write\"},\"content\":"
        "{\"type\":\"string\",\"description\":\"Full file content\"}},"
        "\"required\":[\"path\",\"content\"]}";
    static const char *bash_schema =
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":"
        "\"string\",\"description\":\"Shell command to execute\"}},"
        "\"required\":[\"command\"]}";

    if (reg == NULL)
        return -1;
    if (tools_register(reg, "read_file",
                       "Reads the full content of a file. Use this tool to explore files and understand their contents.", read_schema,
                       0, tool_read_file, builtin_title, NULL) != 0)
        return -1;
    if (tools_register(reg, "write_file",
                       "Create or overwrite a file. Use this tool to create or replace the entire content of a file.",
                       write_schema, 0, tool_write_file, builtin_title,
                       NULL) != 0)
        return -1;
    if (tools_register(reg, "bash",
                       "Execute a system command via a shell. Use this tool to run build commands, "
                       "inspect the filesystem using `ls` or `find`, create temporary files, "
		       "or perform any other task that can be accomplished by executing direct commands on the host system.",
                       bash_schema, 1, tool_bash, builtin_title, NULL) != 0)
        return -1;
    return 0;
}
