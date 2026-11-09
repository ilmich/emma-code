/**
 * @file lua_scripting.c
 * @brief Lua scripting engine bridge: loads user scripts, exposes the
 *        emma.* API and dispatches slash commands and message hooks.
 */

#include "lua_scripting.h"
#include "commands.h"
#include "util.h"

#include "cjson/cJSON.h"

#include <dirent.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* POSIX directory access is not declared by strict ISO C99 headers on
 * this toolchain; declare explicitly instead of enabling feature-test
 * macros (see AGENTS.md portability rules). */
extern DIR *opendir(const char *name);
extern struct dirent *readdir(DIR *dirp);
extern int closedir(DIR *dirp);

/** Size of the internal last-error buffer. */
#define LAST_ERROR_MAX 256

struct lua_bridge {
	lua_State *L;              /**< Lua state (owner of all Lua refs) */
	int hooks_ref;             /**< registry ref: {event = {fn, ...}} */
	int cmds_ref;              /**< registry ref: {name = handler_fn} */
	int tools_ref;             /**< registry ref: {name = handler_fn} */
	tool_registry_t *tools;    /**< borrowed tool registry (may be NULL) */
	tui_t *tui;                /**< borrowed TUI for feedback (may be NULL) */
	lua_script_error_fn error_fn; /**< optional error reporter */
	void *error_ud;            /**< opaque context owned by the registrant */
	char last_error[LAST_ERROR_MAX]; /**< most recent error message */
};

/**
 * Records an error message (truncation-safe), invokes the registered
 * error reporter when present, otherwise surfaces it in the TUI.
 */
static void bridge_error(lua_bridge_t *bridge, const char *fmt, ...)
{
	char msg[LAST_ERROR_MAX];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;

	snprintf(bridge->last_error, sizeof bridge->last_error, "%s", msg);
	if (bridge->error_fn != NULL) {
		bridge->error_fn(msg, bridge->error_ud);
	} else if (bridge->tui != NULL) {
		tui_append_message(bridge->tui, "system", msg);
	}
}

/* ------------------------------------------------------------------ */
/* emma.* Lua API                                                      */
/* ------------------------------------------------------------------ */

/**
 * Copies text into the caller-provided feedback buffer, truncation-safe
 * (snprintf always NUL-terminates).
 */
static void set_feedback(lua_bridge_t *bridge, char *feedback,
			 size_t feedback_len, const char *text)
{
	if (feedback == NULL || feedback_len == 0)
		return;
	if (snprintf(feedback, feedback_len, "%s", text) >= (int)feedback_len)
		bridge_error(bridge, "lua command feedback truncated");
}

/** C handler installed in the command registry for every Lua command. */
static cmd_result_t lua_command_handler(const cmd_invocation_t *inv,
					void *userdata, char *feedback,
					size_t feedback_len)
{
	lua_bridge_t *bridge = userdata;
	lua_State *L;

	if (bridge == NULL || bridge->L == NULL || inv == NULL ||
	    inv->name == NULL)
		return CMD_HANDLED;
	L = bridge->L;

	lua_rawgeti(L, LUA_REGISTRYINDEX, bridge->cmds_ref); /* cmds */
	lua_getfield(L, -1, inv->name);                      /* cmds fn */
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		set_feedback(bridge, feedback, feedback_len,
			     "[lua] command handler missing");
		return CMD_HANDLED;
	}
	lua_pushstring(L, inv->args != NULL ? inv->args : "");
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		const char *msg = lua_tostring(L, -1);

		bridge_error(bridge, "lua error in /%s: %s", inv->name,
			     msg != NULL ? msg : "unknown error");
		lua_pop(L, 2); /* error message + cmds table */
		set_feedback(bridge, feedback, feedback_len,
			     "[lua] command failed (see message above)");
		return CMD_HANDLED;
	}
	if (lua_isstring(L, -1))
		set_feedback(bridge, feedback, feedback_len,
			     lua_tostring(L, -1));
	lua_pop(L, 2); /* result + cmds table */
	return CMD_HANDLED;
}

/** emma.register_command(name, help_or_nil, handler). */
static int l_register_command(lua_State *L)
{
	lua_bridge_t *bridge = lua_touserdata(L, lua_upvalueindex(1));
	const char *name = luaL_checkstring(L, 1);
	const char *help = luaL_optstring(L, 2, NULL);

	luaL_checktype(L, 3, LUA_TFUNCTION);

	/* Keep the handler in the bridge's cmds table; the registry entry
	 * owns nothing and looks the function up by name on dispatch. */
	lua_rawgeti(L, LUA_REGISTRYINDEX, bridge->cmds_ref);
	lua_pushvalue(L, 3);
	lua_setfield(L, -2, name);
	lua_pop(L, 1);

	if (commands_register(name, lua_command_handler, help, bridge) != 0) {
		/* Undo the store to keep the table consistent. */
		lua_rawgeti(L, LUA_REGISTRYINDEX, bridge->cmds_ref);
		lua_pushnil(L);
		lua_setfield(L, -2, name);
		lua_pop(L, 1);
		return luaL_error(L, "cannot register command '%s'", name);
	}
	return 0;
}

/**
 * Converts a Lua value into a cJSON tree (for tool schemas and tool call
 * arguments). Tables become objects, or arrays when they are a sequence
 * (keys 1..n). Unsupported types (functions, threads, cycles) yield NULL.
 *
 * @param L Lua state.
 * @param idx Stack index of the value to convert (absolute for recursion).
 * @return New cJSON item owned by the caller, or NULL on unsupported
 *         input or out of memory.
 */
static cJSON *json_from_lua(lua_State *L, int idx)
{
	int is_array = 1;
	size_t seq_len = 0;
	cJSON *item;

	/* Normalize the index: recursion pushes on the stack and the
	 * lua_next()/rawgeti calls below need a stable absolute index. */
	idx = lua_absindex(L, idx);

	switch (lua_type(L, idx)) {
	case LUA_TSTRING:
		return cJSON_CreateString(lua_tostring(L, idx));
	case LUA_TNUMBER:
		return cJSON_CreateNumber((double)lua_tonumber(L, idx));
	case LUA_TBOOLEAN:
		return cJSON_CreateBool(lua_toboolean(L, idx));
	case LUA_TNIL:
		return cJSON_CreateNull();
	case LUA_TTABLE:
		break;
	default:
		return NULL;
	}

	/* Array detection: a table is a sequence only when its first key is
	 * the integer 1 (and rawlen matches); string keys or an empty table
	 * make it an object. */
	seq_len = lua_rawlen(L, idx);
	lua_pushnil(L);
	if (lua_next(L, idx) != 0) {
		if (lua_tointeger(L, -2) != 1 || seq_len == 0)
			is_array = 0;
		lua_pop(L, 2); /* pop key and value */
	} else {
		is_array = 0; /* empty table: object */
	}

	item = is_array ? cJSON_CreateArray() : cJSON_CreateObject();
	if (item == NULL)
		return NULL;

	if (is_array) {
		for (size_t i = 1; i <= seq_len; i++) {
			lua_rawgeti(L, idx, (lua_Integer)i);
			cJSON *child = json_from_lua(L, -1);

			lua_pop(L, 1);
			if (child == NULL ||
			    cJSON_AddItemToArray(item, child) == 0) {
				cJSON_Delete(child);
				cJSON_Delete(item);
				return NULL;
			}
		}
	} else {
		lua_pushnil(L);
		while (lua_next(L, idx) != 0) {
			const char *key = lua_tostring(L, -2);
			cJSON *child = NULL;
			int ok;

			if (key == NULL) {
				lua_pop(L, 1); /* non-string key: skip value */
				continue;
			}
			child = json_from_lua(L, -1);
			ok = child != NULL &&
			     cJSON_AddItemToObject(item, key, child) != 0;
			lua_pop(L, 1); /* pop value, keep key for lua_next */
			if (!ok) {
				cJSON_Delete(child);
				cJSON_Delete(item);
				return NULL;
			}
		}
	}
	return item;
}

/**
 * Pushes a cJSON tree onto the Lua stack as an equivalent value (objects
 * and arrays become tables, null becomes nil).
 */
static void push_json_as_lua(lua_State *L, const cJSON *item)
{
	if (item == NULL || cJSON_IsNull(item)) {
		lua_pushnil(L);
	} else if (cJSON_IsString(item)) {
		lua_pushstring(L, item->valuestring != NULL
				      ? item->valuestring : "");
	} else if (cJSON_IsNumber(item)) {
		lua_pushnumber(L, (lua_Number)item->valuedouble);
	} else if (cJSON_IsBool(item)) {
		lua_pushboolean(L, cJSON_IsTrue(item));
	} else if (cJSON_IsArray(item)) {
		size_t i = 1;

		lua_newtable(L);
		const cJSON *child = NULL;
		cJSON_ArrayForEach(child, item) {
			push_json_as_lua(L, child);
			lua_rawseti(L, -2, (lua_Integer)i++);
		}
	} else if (cJSON_IsObject(item)) {
		lua_newtable(L);
		const cJSON *child = NULL;
		cJSON_ArrayForEach(child, item) {
			if (child->string == NULL)
				continue;
			push_json_as_lua(L, child);
			lua_setfield(L, -2, child->string);
		}
	} else {
		lua_pushnil(L);
	}
}

/**
 * C handler installed in the tool registry for every Lua tool. Parses the
 * model's JSON arguments into a Lua table and calls the registered Lua
 * function; the returned string (or nil) becomes the tool result.
 */
static int lua_tool_handler(const tool_def_t *tool, const char *args_json,
			    char **result)
{
	lua_bridge_t *bridge = tool != NULL ? tool->userdata : NULL;
	lua_State *L;
	cJSON *args = NULL;
	const char *msg;
	int rc = -1;

	if (bridge == NULL || bridge->L == NULL || tool == NULL ||
	    tool->name == NULL || result == NULL)
		return -1;
	*result = NULL;
	L = bridge->L;

	args = cJSON_Parse(args_json != NULL ? args_json : "");
	if (args == NULL) {
		*result = util_strdup("arguments are not valid JSON");
		return *result != NULL ? -1 : -1;
	}

	lua_rawgeti(L, LUA_REGISTRYINDEX, bridge->tools_ref); /* tools */
	lua_getfield(L, -1, tool->name);                      /* tools fn */
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2);
		cJSON_Delete(args);
		*result = util_strdup("[lua] tool handler missing");
		return -1;
	}
	push_json_as_lua(L, args);
	cJSON_Delete(args);
	args = NULL;
	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		msg = lua_tostring(L, -1);
		bridge_error(bridge, "lua error in tool '%s': %s", tool->name,
			     msg != NULL ? msg : "unknown error");
		lua_pop(L, 2); /* error message + tools table */
		*result = util_strdup("[lua] tool failed (see message above)");
		return -1;
	}
	if (lua_isstring(L, -1)) {
		*result = util_strdup(lua_tostring(L, -1));
		rc = 0;
	} else if (lua_isnil(L, -1)) {
		*result = util_strdup("");
		rc = 0;
	} else {
		*result = util_strdup("[lua] tool returned a non-string value");
		rc = -1;
	}
	lua_pop(L, 2); /* result + tools table */
	if (*result == NULL)
		return -1; /* out of memory */
	return rc;
}

/**
 * emma.register_tool(name, description, schema, handler, opts).
 * opts is optional: { requires_approval = bool } (default false).
 */
static int l_register_tool(lua_State *L)
{
	lua_bridge_t *bridge = lua_touserdata(L, lua_upvalueindex(1));
	const char *name = luaL_checkstring(L, 1);
	const char *description = luaL_checkstring(L, 2);
	cJSON *schema = NULL;
	char *schema_json = NULL;
	int requires_approval = LUA_TOOL_DEFAULT_APPROVAL;
	int rc;

	luaL_checktype(L, 3, LUA_TTABLE);
	luaL_checktype(L, 4, LUA_TFUNCTION);
	if (lua_istable(L, 5)) {
		lua_getfield(L, 5, "requires_approval");
		if (!lua_isnil(L, -1))
			requires_approval = lua_toboolean(L, -1);
		lua_pop(L, 1);
	}

	if (bridge->tools == NULL)
		return luaL_error(L, "no tool registry attached");

	schema = json_from_lua(L, 3);
	if (schema == NULL)
		return luaL_error(L, "invalid schema for tool '%s'", name);
	schema_json = cJSON_PrintUnformatted(schema);
	cJSON_Delete(schema);
	if (schema_json == NULL)
		return luaL_error(L, "out of memory for tool '%s' schema", name);

	/* Keep the handler in the bridge's tools table; the registry entry
	 * looks the function up by name on dispatch. */
	lua_rawgeti(L, LUA_REGISTRYINDEX, bridge->tools_ref);
	lua_pushvalue(L, 4);
	lua_setfield(L, -2, name);
	lua_pop(L, 1);

	rc = tools_register(bridge->tools, name, description, schema_json,
			    requires_approval, lua_tool_handler, NULL, bridge);
	free(schema_json);
	if (rc != 0) {
		/* Undo the store to keep the table consistent. */
		lua_rawgeti(L, LUA_REGISTRYINDEX, bridge->tools_ref);
		lua_pushnil(L);
		lua_setfield(L, -2, name);
		lua_pop(L, 1);
		return luaL_error(L, "cannot register tool '%s'", name);
	}
	return 0;
}

/** emma.register_hook(event, fn): event is pre_message or post_message. */
static int l_register_hook(lua_State *L)
{
	lua_bridge_t *bridge = lua_touserdata(L, lua_upvalueindex(1));
	const char *event = luaL_checkstring(L, 1);

	luaL_checktype(L, 2, LUA_TFUNCTION);
	if (strcmp(event, LUA_HOOK_PRE_MESSAGE) != 0 &&
	    strcmp(event, LUA_HOOK_POST_MESSAGE) != 0)
		return luaL_error(L, "unknown hook event '%s'", event);

	lua_rawgeti(L, LUA_REGISTRYINDEX, bridge->hooks_ref); /* hooks */
	lua_getfield(L, -1, event);                           /* hooks arr */
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);                                /* hooks */
		lua_newtable(L);                              /* hooks new */
		lua_pushvalue(L, -1);                         /* hooks new new */
		lua_setfield(L, -3, event);                   /* hooks new */
	}
	lua_pushvalue(L, 2);                                  /* hooks new fn */
	lua_rawseti(L, -2, (int)(lua_rawlen(L, -2) + 1));     /* hooks new */
	lua_pop(L, 2);
	return 0;
}

/** emma.append_message(role, content): adds a line to the terminal UI. */
static int l_append_message(lua_State *L)
{
	lua_bridge_t *bridge = lua_touserdata(L, lua_upvalueindex(1));
	const char *role = luaL_checkstring(L, 1);
	const char *content = luaL_checkstring(L, 2);

	if (bridge->tui == NULL)
		return luaL_error(L, "no TUI attached to the Lua bridge");
	tui_append_message(bridge->tui, role, content);
	return 0;
}

/** emma.set_status(text_or_nil): sets or clears the TUI status line. */
static int l_set_status(lua_State *L)
{
	lua_bridge_t *bridge = lua_touserdata(L, lua_upvalueindex(1));
	const char *status = luaL_optstring(L, 1, NULL);

	if (bridge->tui == NULL)
		return luaL_error(L, "no TUI attached to the Lua bridge");
	tui_set_status(bridge->tui, status);
	return 0;
}

static const luaL_Reg emma_funcs[] = {
	{ "register_command", l_register_command },
	{ "register_hook",    l_register_hook },
	{ "register_tool",    l_register_tool },
	{ "append_message",   l_append_message },
	{ "set_status",       l_set_status },
	{ NULL, NULL }
};

/** (Re)creates the global emma table with bridge-bound C closures. */
static int setup_emma_table(lua_bridge_t *bridge)
{
	lua_State *L = bridge->L;

	lua_newtable(L);
	lua_pushlightuserdata(L, bridge);
	luaL_setfuncs(L, emma_funcs, 1);
	lua_setglobal(L, "emma");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

int lua_bridge_init(lua_bridge_t **bridge, tui_t *tui)
{
	lua_bridge_t *b;
	int rc = -1;

	if (bridge == NULL)
		return -1;
	*bridge = NULL;
	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return -1; /* nothing to clean up yet */
	b->tui = tui;
	b->L = luaL_newstate();
	if (b->L == NULL)
		goto cleanup;
	luaL_openlibs(b->L);

	lua_newtable(b->L);
	b->hooks_ref = luaL_ref(b->L, LUA_REGISTRYINDEX);
	lua_newtable(b->L);
	b->cmds_ref = luaL_ref(b->L, LUA_REGISTRYINDEX);
	lua_newtable(b->L);
	b->tools_ref = luaL_ref(b->L, LUA_REGISTRYINDEX);
	if (b->hooks_ref == LUA_NOREF || b->cmds_ref == LUA_NOREF ||
	    b->tools_ref == LUA_NOREF)
		goto cleanup;
	if (setup_emma_table(b) != 0)
		goto cleanup;
	*bridge = b;
	b = NULL; /* ownership transferred to *bridge */
	rc = 0;

cleanup:
	if (b != NULL && b->L != NULL)
		lua_close(b->L);
	free(b);
	return rc;
}

void lua_bridge_set_error_reporter(lua_bridge_t *bridge,
				   lua_script_error_fn fn, void *userdata)
{
	if (bridge == NULL)
		return;
	bridge->error_fn = fn;
	bridge->error_ud = userdata;
}

void lua_bridge_set_tool_registry(lua_bridge_t *bridge, tool_registry_t *reg)
{
	if (bridge == NULL)
		return;
	bridge->tools = reg;
}

void lua_bridge_free(lua_bridge_t *bridge)
{
	if (bridge == NULL)
		return;
	if (bridge->L != NULL) {
		luaL_unref(bridge->L, LUA_REGISTRYINDEX, bridge->hooks_ref);
		luaL_unref(bridge->L, LUA_REGISTRYINDEX, bridge->cmds_ref);
		luaL_unref(bridge->L, LUA_REGISTRYINDEX, bridge->tools_ref);
		lua_close(bridge->L);
	}
	free(bridge);
}

const char *lua_bridge_last_error(const lua_bridge_t *bridge)
{
	if (bridge == NULL)
		return "";
	return bridge->last_error;
}

/* ------------------------------------------------------------------ */
/* Script loading                                                      */
/* ------------------------------------------------------------------ */

/** qsort comparator for collected script file names (alphabetical). */
static int cmp_names(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/** Frees the collected file-name array. */
static void free_names(char **names, size_t count)
{
	for (size_t i = 0; i < count; i++)
		free(names[i]);
	free(names);
}

/** Runs one loaded script chunk; errors are reported, never fatal. */
static void run_script(lua_bridge_t *bridge, const char *path)
{
	lua_State *L = bridge->L;

	if (luaL_loadfile(L, path) != LUA_OK) {
		const char *msg = lua_tostring(L, -1);

		bridge_error(bridge, "lua load error: %s",
			     msg != NULL ? msg : "unknown error");
		lua_pop(L, 1);
		return;
	}
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		const char *msg = lua_tostring(L, -1);

		bridge_error(bridge, "lua script error: %s",
			     msg != NULL ? msg : "unknown error");
		lua_pop(L, 1);
	}
}

int lua_bridge_load_scripts(lua_bridge_t *bridge, const char *dir)
{
	DIR *d;
	struct dirent *ent;
	char **names = NULL;
	size_t count = 0;
	size_t cap = 0;
	int rc = 0;

	if (bridge == NULL || bridge->L == NULL || dir == NULL || *dir == '\0')
		return -1;

	setup_emma_table(bridge); /* refresh in case of repeated loads */

	d = opendir(dir);
	if (d == NULL)
		return 0; /* no directory: nothing to load, not an error */

	while ((ent = readdir(d)) != NULL) {
		const char *name = ent->d_name;
		size_t len = strlen(name);
		char **grown;
		char *copy;

		if (len < 4 || strcmp(name + len - 4, ".lua") != 0)
			continue;
		if (name[0] == '.')
			continue;
		copy = util_strdup(name);
		if (copy == NULL) {
			rc = -1;
			goto cleanup;
		}
		if (count == cap) {
			size_t ncap = cap == 0 ? 8 : cap * 2;

			grown = realloc(names, ncap * sizeof(*names));
			if (grown == NULL) {
				free(copy);
				rc = -1;
				goto cleanup;
			}
			names = grown;
			cap = ncap;
		}
		names[count++] = copy;
	}

	if (count > 1)
		qsort(names, count, sizeof(*names), cmp_names);

	for (size_t i = 0; i < count; i++) {
		size_t dlen = strlen(dir);
		size_t nlen = strlen(names[i]);
		char *path = malloc(dlen + 1 + nlen + 1);

		if (path == NULL) {
			rc = -1;
			goto cleanup;
		}
		if (snprintf(path, dlen + 2 + nlen, "%s/%s", dir, names[i]) >=
		    (int)(dlen + 2 + nlen)) {
			free(path);
			rc = -1;
			goto cleanup;
		}
		run_script(bridge, path);
		free(path);
	}

cleanup:
	closedir(d);
	free_names(names, count);
	if (rc != 0)
		bridge_error(bridge, "out of memory loading scripts");
	return rc;
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

int lua_bridge_apply_hook(lua_bridge_t *bridge, const char *event,
			  const char *payload, char **out)
{
	lua_State *L;
	size_t count;
	char *acc = NULL;
	const char *current = payload;
	int rc = -1;

	if (bridge == NULL || bridge->L == NULL || event == NULL ||
	    payload == NULL || out == NULL)
		return -1;
	*out = NULL;
	if (strcmp(event, LUA_HOOK_PRE_MESSAGE) != 0 &&
	    strcmp(event, LUA_HOOK_POST_MESSAGE) != 0)
		return -1;

	L = bridge->L;
	lua_rawgeti(L, LUA_REGISTRYINDEX, bridge->hooks_ref); /* hooks */
	lua_getfield(L, -1, event);                           /* hooks arr */
	if (lua_istable(L, -1)) {
		count = lua_rawlen(L, -1);
		for (size_t i = 1; i <= count; i++) {
			lua_rawgeti(L, -1, (lua_Integer)i);       /* hooks arr fn */
			lua_pushstring(L, current);
			if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
				const char *msg = lua_tostring(L, -1);

				bridge_error(bridge, "lua hook '%s' error: %s", event,
					     msg != NULL ? msg : "unknown error");
				lua_pop(L, 1); /* error message */
				continue;      /* pass-through on hook failure */
			}
			if (lua_isstring(L, -1)) {
				const char *r = lua_tostring(L, -1);

				if (strcmp(r, current) != 0) {
					char *copy = util_strdup(r);

					if (copy == NULL) {
						lua_pop(L, 1); /* hook result */
						goto cleanup;
					}
					free(acc);
					acc = copy;
					current = acc;
				}
			}
			lua_pop(L, 1); /* hook result */
		}
	}

	/* Only surface a result when it actually differs from the input. */
	if (acc != NULL && strcmp(acc, payload) == 0) {
		free(acc);
		acc = NULL;
	}
	*out = acc;
	acc = NULL; /* ownership transferred to *out */
	rc = 0;

cleanup:
	lua_pop(L, 2); /* arr + hooks */
	free(acc);
	return rc;
}
