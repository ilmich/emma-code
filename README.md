# emma-code

A minimal terminal UI agent for coding assistance using OpenAI's Chat Completion API.

## Overview

Emma-Code is a lightweight terminal-based interface that allows you to interact with an AI assistant for coding tasks. It provides a seamless experience for asking questions, debugging, and getting code suggestions in a terminal environment.

## Features

- Terminal UI for coding assistance
- Slash commands with a runtime command registry (built-in `/exit`)
- Lua scripting engine: user scripts can register commands, drive the UI
  and hook into the message flow (`pre_message` / `post_message`)
- Conversation history management with configurable message limits
- HTTP client for OpenAI API integration
- TOML configuration file support (minimal, dependency-free parser)
- UTF-8 support for international characters

## Tech Stack

- Language: C (C99)
- Build system: Make
- HTTP client: libcurl
- JSON parsing: cJSON
- Scripting: Lua 5.4 (system liblua, 5.3 fallback)

## Build and Usage

### Prerequisites

- libcurl development package
- cJSON development package
- Lua development package (`liblua5.4-dev` on Debian/Ubuntu, `lua-devel` on Fedora)
- A C99 compiler (gcc)

### Build

```
make
```

### Configure

Emma reads its configuration from `$XDG_CONFIG_HOME/emma/config.toml`, falling back to `~/.config/emma/config.toml` when `XDG_CONFIG_HOME` is unset:

```toml
base_url = "https://api.openai.com/v1"  # mandatory
api_key = "your_api_key_here"           # optional
model = "gpt-4o-mini"                   # optional, default: gpt-3.5-turbo
temperature = 0.7                       # optional, default: 0.7
scripts_dir = "~/.config/emma/scripts"  # optional, Lua scripts directory
```

### Run

```
./emma-code
```

### Clean

```
make clean
```

## Scripting Lua

Emma loads every `*.lua` file (in alphabetical order) from the scripts
directory at startup: `scripts_dir` from `config.toml`, defaulting to
`$XDG_CONFIG_HOME/emma/scripts` or `~/.config/emma/scripts`. Scripts can
register slash commands, interact with the UI and hook into the message
flow:

```lua
-- ~/.config/emma/scripts/hello.lua
emma.register_command("hello", "greets you", function(args)
  return "hello, " .. (args ~= "" and args or "world")
end)

emma.register_hook("pre_message", function(text)
  return text .. " (answer in english)"   -- sent to the LLM instead
end)

emma.register_hook("post_message", function(text)
  return text .. "\n--- via emma ---"      -- shown and stored instead
end)
```

`pre_message` fires when you press enter (before the message is stored or
displayed); `post_message` fires when the LLM completes its response. A
hook returning `nil` leaves the text unchanged. See
[doc/lua_api.md](doc/lua_api.md) for the full API reference.

## Repository Structure

- `include/` – public header files (one per source module)
- `src/` – source files (`main.c`, `http_client.c`, `openai_client.c`, `session_context.c`, `tui.c`, `toml.c`, `commands.c`, `lua_scripting.c`)
- `tests/` – unit tests (`test_*.c`) and the end-to-end test runner (`run_tests.sh`, using `mock_server.py`)
- `doc/` – documentation (`lua_api.md`, `examples/`)
- `build/` – object files and binaries (ignored by git)
- `Makefile` – build script
- `build.sh` – bash build script alternative to make

## Testing

Compile and run the unit tests (the network suite uses a local mock OpenAI server):

```
make test
```

Verify repository layout rules (every `src/*.c` except `main.c` must have a matching `include/*.h`, and a matching `tests/test_*.c` except for `tui.c`):

```
make check
```

## License

MIT — see [LICENSE](LICENSE).
