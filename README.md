# emma-code

A minimal terminal UI agent for coding assistance using OpenAI's Chat Completion API.

## Overview

Emma-Code is a lightweight terminal-based interface that allows you to interact with an AI assistant for coding tasks. It provides a seamless experience for asking questions, debugging, and getting code suggestions in a terminal environment.

## Features

- Terminal UI for coding assistance
- Conversation history management with configurable message limits
- HTTP client for OpenAI API integration
- TOML configuration file support (minimal, dependency-free parser)
- UTF-8 support for international characters

## Tech Stack

- Language: C (C99)
- Build system: Make
- HTTP client: libcurl
- JSON parsing: cJSON

## Build and Usage

### Prerequisites

- libcurl development package
- cJSON development package
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
```

### Run

```
./emma-code
```

### Clean

```
make clean
```

## Repository Structure

- `include/` – public header files (one per source module)
- `src/` – source files (`main.c`, `http_client.c`, `openai_client.c`, `session_context.c`, `tui.c`, `toml.c`)
- `tests/` – unit tests (`test_*.c`) and the end-to-end test runner (`run_tests.sh`, using `mock_server.py`)
- `doc/` – documentation
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
