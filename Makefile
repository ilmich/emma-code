CC := gcc

# All external dependencies resolved through pkg-config: libcurl, cJSON
# and the Lua runtime (system liblua, 5.4 preferred, 5.3 fallback).
DEP_MISSING := $(shell for p in libcurl libcjson; do pkg-config --exists $$p 2>/dev/null || echo $$p; done)
ifneq ($(DEP_MISSING),)
$(error pkg-config could not find: $(DEP_MISSING) - install the corresponding development packages)
endif
LUA_PKG := $(shell for p in lua5.4 lua5.3 lua; do pkg-config --exists $$p && echo $$p && break; done)
ifeq ($(LUA_PKG),)
$(error Lua development files not found: install liblua5.4-dev (Debian/Ubuntu) or lua-devel (Fedora))
endif
CURL_CFLAGS := $(shell pkg-config --cflags libcurl)
CURL_LIBS := $(shell pkg-config --libs libcurl)
CJSON_CFLAGS := $(shell pkg-config --cflags libcjson)
CJSON_LIBS := $(shell pkg-config --libs libcjson)
LUA_CFLAGS := $(shell pkg-config --cflags $(LUA_PKG))
LUA_LIBS := $(shell pkg-config --libs $(LUA_PKG))

CFLAGS := -std=c99 -Wall -Wextra -O2 -Iinclude -fanalyzer $(CURL_CFLAGS) $(CJSON_CFLAGS) $(LUA_CFLAGS)
SRCDIR := src
BUILDDIR := build
TARGET := emma-code
TESTDIR := tests
TESTBIN := $(BUILDDIR)/tests

SOURCES := $(SRCDIR)/main.c $(SRCDIR)/http_client.c $(SRCDIR)/openai_client.c \
	$(SRCDIR)/session_context.c $(SRCDIR)/tui.c $(SRCDIR)/toml.c \
	$(SRCDIR)/commands.c $(SRCDIR)/lua_scripting.c $(SRCDIR)/agent_loop.c \
	$(SRCDIR)/util.c $(SRCDIR)/markdown.c $(SRCDIR)/tools.c
OBJECTS := $(SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) $(CURL_LIBS) $(CJSON_LIBS) $(LUA_LIBS) -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR) $(TARGET)

# Compile and run the unit tests. The network suite uses the local mock
# OpenAI server started by tests/run_tests.sh.
test: $(TESTBIN)/test_session_context $(TESTBIN)/test_openai_client \
	$(TESTBIN)/test_http_client $(TESTBIN)/test_toml $(TESTBIN)/test_commands \
	$(TESTBIN)/test_lua_scripting $(TESTBIN)/test_tui $(TESTBIN)/test_agent_loop \
	$(TESTBIN)/test_util $(TESTBIN)/test_markdown $(TESTBIN)/test_tools
	$(TESTDIR)/run_tests.sh

# End-to-end routing checks against the local mock OpenAI server.
e2e: $(TESTBIN)/test_openai_client $(TESTBIN)/test_agent_loop
	$(TESTDIR)/e2e.sh

# Enforce the repo layout rules: every src/*.c (except main.c) must have a
# matching include/*.h, and every src/*.c (except main.c) must have a
# tests/test_*.c.
check:
	@status=0; \
	for f in $(SRCDIR)/*.c; do \
		base=$$(basename $$f .c); \
		if [ "$$base" != "main" ] && [ ! -f include/$$base.h ]; then \
			echo "check: missing include/$$base.h for $$f"; status=1; \
		fi; \
		if [ "$$base" != "main" ] && [ ! -f $(TESTDIR)/test_$$base.c ]; then \
			echo "check: missing $(TESTDIR)/test_$$base.c for $$f"; status=1; \
		fi; \
	done; \
	if [ $$status -eq 0 ]; then echo "check: OK"; fi; \
	exit $$status

$(TESTBIN):
	mkdir -p $(TESTBIN)

$(TESTBIN)/test_session_context: $(TESTDIR)/test_session_context.c $(TESTDIR)/test.h \
		$(BUILDDIR)/session_context.o $(BUILDDIR)/openai_client.o \
		$(BUILDDIR)/http_client.o $(BUILDDIR)/util.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_session_context.c \
		$(BUILDDIR)/session_context.o $(BUILDDIR)/openai_client.o \
		$(BUILDDIR)/http_client.o $(BUILDDIR)/util.o \
		-o $@ $(CJSON_LIBS) $(CURL_LIBS)

$(TESTBIN)/test_openai_client: $(TESTDIR)/test_openai_client.c $(TESTDIR)/test.h $(BUILDDIR)/openai_client.o $(BUILDDIR)/http_client.o $(BUILDDIR)/util.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_openai_client.c $(BUILDDIR)/openai_client.o $(BUILDDIR)/http_client.o $(BUILDDIR)/util.o $(CURL_LIBS) $(CJSON_LIBS) -o $@

$(TESTBIN)/test_http_client: $(TESTDIR)/test_http_client.c $(TESTDIR)/test.h $(BUILDDIR)/http_client.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_http_client.c $(BUILDDIR)/http_client.o $(CURL_LIBS) -o $@

$(TESTBIN)/test_toml: $(TESTDIR)/test_toml.c $(TESTDIR)/test.h $(BUILDDIR)/toml.o $(BUILDDIR)/util.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_toml.c $(BUILDDIR)/toml.o $(BUILDDIR)/util.o -o $@

$(TESTBIN)/test_util: $(TESTDIR)/test_util.c $(TESTDIR)/test.h $(BUILDDIR)/util.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_util.c $(BUILDDIR)/util.o -o $@

$(TESTBIN)/test_commands: $(TESTDIR)/test_commands.c $(TESTDIR)/test.h $(BUILDDIR)/commands.o $(BUILDDIR)/util.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_commands.c $(BUILDDIR)/commands.o $(BUILDDIR)/util.o -o $@

$(TESTBIN)/test_lua_scripting: $(TESTDIR)/test_lua_scripting.c $(TESTDIR)/test.h \
		$(BUILDDIR)/lua_scripting.o $(BUILDDIR)/commands.o $(BUILDDIR)/tui.o \
		$(BUILDDIR)/markdown.o $(BUILDDIR)/util.o $(BUILDDIR)/tools.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_lua_scripting.c \
		$(BUILDDIR)/lua_scripting.o $(BUILDDIR)/commands.o $(BUILDDIR)/tui.o \
		$(BUILDDIR)/markdown.o $(BUILDDIR)/util.o $(BUILDDIR)/tools.o \
		-o $@ $(LUA_LIBS) $(CJSON_LIBS)

$(TESTBIN)/test_tools: $(TESTDIR)/test_tools.c $(TESTDIR)/test.h \
		$(BUILDDIR)/tools.o $(BUILDDIR)/util.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_tools.c \
		$(BUILDDIR)/tools.o $(BUILDDIR)/util.o -o $@ $(CJSON_LIBS)

$(TESTBIN)/test_tui: $(TESTDIR)/test_tui.c $(TESTDIR)/test.h $(BUILDDIR)/tui.o \
		$(BUILDDIR)/markdown.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_tui.c $(BUILDDIR)/tui.o \
		$(BUILDDIR)/markdown.o -o $@

$(TESTBIN)/test_markdown: $(TESTDIR)/test_markdown.c $(TESTDIR)/test.h \
		$(BUILDDIR)/markdown.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_markdown.c \
		$(BUILDDIR)/markdown.o -o $@

$(TESTBIN)/test_agent_loop: $(TESTDIR)/test_agent_loop.c $(TESTDIR)/test.h \
		$(BUILDDIR)/agent_loop.o $(BUILDDIR)/openai_client.o $(BUILDDIR)/http_client.o \
		$(BUILDDIR)/session_context.o $(BUILDDIR)/tui.o $(BUILDDIR)/markdown.o \
		$(BUILDDIR)/util.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_agent_loop.c \
		$(BUILDDIR)/agent_loop.o $(BUILDDIR)/openai_client.o $(BUILDDIR)/http_client.o \
		$(BUILDDIR)/session_context.o $(BUILDDIR)/tui.o $(BUILDDIR)/markdown.o \
		$(BUILDDIR)/util.o \
		-o $@ $(CURL_LIBS) $(CJSON_LIBS)

.PHONY: all clean test e2e check