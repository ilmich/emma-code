/**
 * @file lua_scripting.c
 * @brief Lua scripting engine bridge: loads user scripts, exposes the
 *        emma.* API and dispatches slash commands and message hooks.
 */

#include "lua_scripting.h"
#include "commands.h"

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
	tui_t *tui;                /**< borrowed TUI for feedback (may be NULL) */
	lua_script_error_fn error_fn; /**< optional error reporter */
	void *error_ud;            /**< opaque context owned by the registrant */
	char last_error[LAST_ERROR_MAX]; /**< most recent error message */
};

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = malloc(n);

	if (d != NULL)
		memcpy(d, s, n);
	return d;
}

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

	if (bridge == NULL)
		return -1;
	*bridge = NULL;
	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return -1;
	b->tui = tui;
	b->L = luaL_newstate();
	if (b->L == NULL) {
		free(b);
		return -1;
	}
	luaL_openlibs(b->L);

	lua_newtable(b->L);
	b->hooks_ref = luaL_ref(b->L, LUA_REGISTRYINDEX);
	lua_newtable(b->L);
	b->cmds_ref = luaL_ref(b->L, LUA_REGISTRYINDEX);
	if (b->hooks_ref == LUA_NOREF || b->cmds_ref == LUA_NOREF) {
		lua_close(b->L);
		free(b);
		return -1;
	}
	if (setup_emma_table(b) != 0) {
		lua_close(b->L);
		free(b);
		return -1;
	}
	*bridge = b;
	return 0;
}

void lua_bridge_set_error_reporter(lua_bridge_t *bridge,
				   lua_script_error_fn fn, void *userdata)
{
	if (bridge == NULL)
		return;
	bridge->error_fn = fn;
	bridge->error_ud = userdata;
}

void lua_bridge_free(lua_bridge_t *bridge)
{
	if (bridge == NULL)
		return;
	if (bridge->L != NULL) {
		luaL_unref(bridge->L, LUA_REGISTRYINDEX, bridge->hooks_ref);
		luaL_unref(bridge->L, LUA_REGISTRYINDEX, bridge->cmds_ref);
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
		copy = xstrdup(name);
		if (copy == NULL) {
			rc = -1;
			break;
		}
		if (count == cap) {
			size_t ncap = cap == 0 ? 8 : cap * 2;

			grown = realloc(names, ncap * sizeof(*names));
			if (grown == NULL) {
				free(copy);
				rc = -1;
				break;
			}
			names = grown;
			cap = ncap;
		}
		names[count++] = copy;
	}
	closedir(d);

	if (rc != 0) {
		free_names(names, count);
		bridge_error(bridge, "out of memory loading scripts");
		return -1;
	}
	if (count > 1)
		qsort(names, count, sizeof(*names), cmp_names);

	for (size_t i = 0; i < count && rc == 0; i++) {
		size_t dlen = strlen(dir);
		size_t nlen = strlen(names[i]);
		char *path = malloc(dlen + 1 + nlen + 1);

		if (path == NULL) {
			rc = -1;
			break;
		}
		if (snprintf(path, dlen + 2 + nlen, "%s/%s", dir, names[i]) >=
		    (int)(dlen + 2 + nlen)) {
			free(path);
			rc = -1;
			break;
		}
		run_script(bridge, path);
		free(path);
	}
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
	if (!lua_istable(L, -1)) {
		lua_pop(L, 2);
		return 0;
	}
	count = lua_rawlen(L, -1);
	for (size_t i = 1; i <= count; i++) {
		lua_rawgeti(L, -1, (lua_Integer)i);           /* hooks arr fn */
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
				char *copy = xstrdup(r);

				if (copy == NULL) {
					lua_pop(L, 3);
					free(acc);
					return -1;
				}
				free(acc);
				acc = copy;
				current = acc;
			}
		}
		lua_pop(L, 1); /* hook result */
	}
	lua_pop(L, 2); /* arr + hooks */

	/* Only surface a result when it actually differs from the input. */
	if (acc != NULL && strcmp(acc, payload) == 0) {
		free(acc);
		acc = NULL;
	}
	*out = acc;
	return 0;
}
