CC := gcc

# Lua runtime: system liblua via pkg-config (5.4 preferred, 5.3 fallback).
LUA_PKG := $(shell for p in lua5.4 lua5.3 lua; do pkg-config --exists $$p && echo $$p && break; done)
ifeq ($(LUA_PKG),)
$(error Lua development files not found: install liblua5.4-dev (Debian/Ubuntu) or lua-devel (Fedora))
endif
LUA_CFLAGS := $(shell pkg-config --cflags $(LUA_PKG))
LUA_LIBS := $(shell pkg-config --libs $(LUA_PKG))

CFLAGS := -std=c99 -Wall -Wextra -O2 -Iinclude -fanalyzer $(LUA_CFLAGS)
SRCDIR := src
BUILDDIR := build
TARGET := emma-code
TESTDIR := tests
TESTBIN := $(BUILDDIR)/tests

SOURCES := $(SRCDIR)/main.c $(SRCDIR)/http_client.c $(SRCDIR)/openai_client.c \
	$(SRCDIR)/session_context.c $(SRCDIR)/tui.c $(SRCDIR)/toml.c \
	$(SRCDIR)/commands.c $(SRCDIR)/lua_scripting.c
OBJECTS := $(SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/%.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -lcurl -lcjson $(LUA_LIBS) -o $@

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
	$(TESTBIN)/test_lua_scripting
	$(TESTDIR)/run_tests.sh

# Enforce the repo layout rules: every src/*.c (except main.c) must have a
# matching include/*.h, and every src/*.c (except main.c and tui.c, a
# terminal-driven module excluded from automated unit tests) must have a
# tests/test_*.c.
check:
	@status=0; \
	for f in $(SRCDIR)/*.c; do \
		base=$$(basename $$f .c); \
		if [ "$$base" != "main" ] && [ ! -f include/$$base.h ]; then \
			echo "check: missing include/$$base.h for $$f"; status=1; \
		fi; \
		if [ "$$base" != "main" ] && [ "$$base" != "tui" ] \
				&& [ ! -f $(TESTDIR)/test_$$base.c ]; then \
			echo "check: missing $(TESTDIR)/test_$$base.c for $$f"; status=1; \
		fi; \
	done; \
	if [ $$status -eq 0 ]; then echo "check: OK"; fi; \
	exit $$status

$(TESTBIN):
	mkdir -p $(TESTBIN)

$(TESTBIN)/test_session_context: $(TESTDIR)/test_session_context.c $(TESTDIR)/test.h $(BUILDDIR)/session_context.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_session_context.c $(BUILDDIR)/session_context.o -o $@

$(TESTBIN)/test_openai_client: $(TESTDIR)/test_openai_client.c $(TESTDIR)/test.h $(BUILDDIR)/openai_client.o $(BUILDDIR)/http_client.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_openai_client.c $(BUILDDIR)/openai_client.o $(BUILDDIR)/http_client.o -lcurl -lcjson -o $@

$(TESTBIN)/test_http_client: $(TESTDIR)/test_http_client.c $(TESTDIR)/test.h $(BUILDDIR)/http_client.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_http_client.c $(BUILDDIR)/http_client.o -lcurl -o $@

$(TESTBIN)/test_toml: $(TESTDIR)/test_toml.c $(TESTDIR)/test.h $(BUILDDIR)/toml.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_toml.c $(BUILDDIR)/toml.o -o $@

$(TESTBIN)/test_commands: $(TESTDIR)/test_commands.c $(TESTDIR)/test.h $(BUILDDIR)/commands.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_commands.c $(BUILDDIR)/commands.o -o $@

$(TESTBIN)/test_lua_scripting: $(TESTDIR)/test_lua_scripting.c $(TESTDIR)/test.h \
		$(BUILDDIR)/lua_scripting.o $(BUILDDIR)/commands.o $(BUILDDIR)/tui.o | $(TESTBIN)
	$(CC) $(CFLAGS) -I$(TESTDIR) $(TESTDIR)/test_lua_scripting.c \
		$(BUILDDIR)/lua_scripting.o $(BUILDDIR)/commands.o $(BUILDDIR)/tui.o -o $@ $(LUA_LIBS)

.PHONY: all clean test check