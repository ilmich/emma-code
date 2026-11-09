/**
 * @file tools.h
 * @brief Runtime tool registry: built-in tools plus tools registered by
 *        Lua scripts, announced to the model via the OpenAI "tools" array.
 *
 * A tool definition couples a name/description/JSON-schema triple (what
 * the model sees) with a C handler (what executes). The requires_approval
 * flag marks side-effectful tools: the dispatcher in main.c asks the user
 * for confirmation before running them (mechanism is generic; today only
 * the bash built-in sets the flag).
 */

#ifndef TOOLS_H
#define TOOLS_H

#include <stddef.h>

/** Maximum tool name length (excluding the NUL). */
#define TOOLS_NAME_MAX 64

/** Maximum serialized tool output handed back to the model. */
#define TOOLS_RESULT_MAX (64 * 1024)

/** Headroom kept in the output buffer for trailing status notices. */
#define TOOLS_NOTICE_ROOM 64

/** A registered tool definition. Fields are owned by the registry. */
typedef struct tool_def {
    char *name;             /**< function name shown to the model */
    char *description;      /**< function description shown to the model */
    char *parameters;       /**< JSON schema of the arguments (string) */
    int requires_approval;  /**< nonzero: dispatcher must confirm with the user */
    /** Executes the tool. See tool_handler_fn for the contract. */
    int (*handler)(const struct tool_def *tool, const char *arguments_json,
                   char **result);
    void *userdata;         /**< opaque context handed back to the handler */
} tool_def_t;

/**
 * Signature of a tool handler.
 *
 * Parses arguments_json (a JSON string produced by the model), performs
 * the action and writes a human/LLM-readable output into *result (malloc,
 * owned by the caller of the handler). Returns 0 when the tool succeeded
 * (*result is its output) or -1 when it failed (*result is an error
 * description that is still sent back to the model). Handlers never free
 * memory they do not own.
 */
typedef int (*tool_handler_fn)(const tool_def_t *tool,
                               const char *arguments_json, char **result);

/** Opaque registry holding the tool definitions. */
typedef struct tool_registry tool_registry_t;

/**
 * Initializes an empty tool registry.
 *
 * @param reg Receives the new registry on success (free with
 *            tools_registry_free()).
 * @return 0 on success, -1 on invalid arguments or out of memory.
 */
int tools_registry_init(tool_registry_t **reg);

/** Releases all registry resources. Safe to call with NULL. */
void tools_registry_free(tool_registry_t *reg);

/**
 * Registers a tool. name, description and parameters are copied into the
 * registry; userdata stays owned by the caller. Duplicate names are
 * rejected.
 *
 * @return 0 on success, -1 on invalid arguments, duplicate name or OOM.
 */
int tools_register(tool_registry_t *reg, const char *name,
                   const char *description, const char *parameters_schema,
                   int requires_approval, tool_handler_fn handler,
                   void *userdata);

/** Looks a tool up by name. Returns NULL when not found. */
const tool_def_t *tools_find(const tool_registry_t *reg, const char *name);

/** Returns the i-th registered tool (NULL when out of range). */
const tool_def_t *tools_at(const tool_registry_t *reg, size_t i);

/** Returns the number of registered tools (0 when reg is NULL). */
size_t tools_count(const tool_registry_t *reg);

/**
 * Serializes the registry into the OpenAI "tools" JSON array:
 * [{"type":"function","function":{"name":...,"description":...,
 * "parameters":...}}, ...].
 *
 * @return malloc'ed JSON string owned by the caller, or NULL when the
 *         registry is NULL, empty or on allocation failure.
 */
char *tools_build_json(const tool_registry_t *reg);

/**
 * Registers the built-in tools: read_file, write_file and bash
 * (bash sets requires_approval).
 *
 * @return 0 on success, -1 on failure (registry left usable but possibly
 *         partially populated).
 */
int tools_register_builtins(tool_registry_t *reg);

#endif /* TOOLS_H */
