/**
 * @file lua_scripting.h
 * @brief Lua scripting engine bridge: user scripts extending emma-code.
 *
 * Scripts are plain *.lua files loaded from a configurable directory
 * (default ~/.config/emma/scripts). Each script can:
 *   - register slash commands through the C command registry;
 *   - interact with the terminal UI;
 *   - hook into the single-message flow (pre_message / post_message).
 */

#ifndef LUA_SCRIPTING_H
#define LUA_SCRIPTING_H

#include <stddef.h>
#include "tui.h"
#include "tools.h"

typedef struct lua_bridge lua_bridge_t;

/** Hook fired when the user submits a message (before session storage). */
#define LUA_HOOK_PRE_MESSAGE "pre_message"

/** Hook fired when the LLM completes its response (before display). */
#define LUA_HOOK_POST_MESSAGE "post_message"

/** Default requires_approval for Lua tools when opts omits the flag. */
#define LUA_TOOL_DEFAULT_APPROVAL 0

/**
 * Callback used to report script load/runtime errors to the embedding UI.
 * Invoked with a human-readable message; must not free anything owned
 * by the bridge.
 */
typedef void (*lua_script_error_fn)(const char *message, void *userdata);

/**
 * Initializes a Lua bridge: creates the lua_State, opens the standard
 * libraries and prepares the internal hook/command tables.
 *
 * @param bridge Receives the new bridge on success.
 * @param tui    Terminal UI used for user-visible feedback (may be NULL,
 *               e.g. in unit tests).
 * @return 0 on success, -1 on failure (bridge set to NULL).
 */
int lua_bridge_init(lua_bridge_t **bridge, tui_t *tui);

/**
 * Sets the error reporter invoked for script load and runtime errors.
 * When never set, errors are only recorded internally (see
 * lua_bridge_last_error()).
 */
void lua_bridge_set_error_reporter(lua_bridge_t *bridge,
				   lua_script_error_fn fn, void *userdata);

/**
 * Attaches the shared tool registry: emma.register_tool() registers Lua
 * handlers into it. The registry is borrowed, must outlive the bridge and
 * must be set before scripts are loaded for register_tool to work.
 *
 * @param bridge Bridge to attach to (may be NULL: no-op).
 * @param reg    Tool registry (borrowed; may be NULL to disable).
 */
void lua_bridge_set_tool_registry(lua_bridge_t *bridge, tool_registry_t *reg);

/**
 * Releases the bridge and its Lua state. Safe to call with NULL.
 */
void lua_bridge_free(lua_bridge_t *bridge);

/**
 * Loads every *.lua file in dir (alphabetical order, hidden files
 * skipped). A missing or unreadable directory is not an error: nothing
 * is loaded and 0 is returned. Errors in a single script (syntax,
 * top-level runtime error) are reported through the error reporter and
 * skipped; only fatal conditions (invalid arguments, out of memory)
 * return -1.
 *
 * @return 0 on success, -1 on fatal failure.
 */
int lua_bridge_load_scripts(lua_bridge_t *bridge, const char *dir);

/**
 * Applies the hook registered for event ("pre_message" or
 * "post_message") to the string payload. Multiple hooks for the same
 * event are chained: each receives the previous hook's output; a hook
 * returning nil leaves the payload unchanged; a hook raising an error
 * is reported and skipped (pass-through).
 *
 * @param out     Receives a malloc'ed NUL-terminated string owned by the
 *                caller, but only when the final result differs from the
 *                payload; otherwise it is set to NULL.
 * @return 0 on success (including "no hook registered"), -1 on invalid
 *         arguments or out of memory.
 */
int lua_bridge_apply_hook(lua_bridge_t *bridge, const char *event,
			  const char *payload, char **out);

/**
 * Returns the most recent error message recorded for this bridge
 * ("" when none). Useful in tests and when no reporter is attached.
 */
const char *lua_bridge_last_error(const lua_bridge_t *bridge);

#endif /* LUA_SCRIPTING_H */
