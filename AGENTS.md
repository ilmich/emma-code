# emma-code

A minimal agent tui for coding.

## Tech stack
- Language: C (C99)
- Build system: Make

## Package manager & commands
- Build: `make` or `build.sh`
- Test: `make test` (unit tests in `tests/test_*.c`, end-to-end in `tests/e2e.sh`; known bugs are `XFAIL`)
- Lint: not configured
- Clean: `make clean`

## Repository structure
- `/include` – Directory housing all public header files for the project.
- `/src` – source files
- `/tests` – test files (none yet)
- `/doc` – documentation
- `/build` – object files and binaries (ignored by git)
- `Makefile` – make build script
- `build.sh` – bash build script alternative to make
- `.gitignore` – ignore build and executable

## Core Components
- `src/main.c`: The primary source file containing the main application logic and TUI setup.
- `include/http_client.h`: Defines the C interface for performing HTTP requests using libcurl, supporting both standard and streaming responses.
- `include/openai_client.h`: Defines the C interface for interacting with the OpenAI Chat Completion API, including message structures and response handling.
- `include/session_context.h`: Defines the C interface for managing session context.
- `Makefile`: Defines the build targets and compilation rules for the project.

## Conventions

### Coding Rules
- Every `*.c` file must have a corresponding `include/*.h` file, except `main.c`.
- Header files should be placed in the `include/` directory or alongside the source file in the same directory.
- The build system should enforce this rule (e.g., via a make check or pre-commit hook).
- Prefer centralize and commonize code: avoid duplication, reuse existing modules, place shared code in /include.\n- Function names must follow the `foo_bla` standard and never `FooBla`.
- Functions that should not be visible outside the compilation unit must be declared as static.
- Error Handling: Check the return value of every call that can fail; free all previously allocated resources on failure and return an error code or report to `stderr`. For pointer-returning functions (`malloc`, `realloc`, `calloc`, `strdup`, `getenv`, `cJSON_Create*`, `cJSON_Print`, etc.), check for `NULL` immediately before use.
- Ownership and double-free prevention: Every dynamically allocated resource has exactly one owner responsible for freeing it. Never free a resource you do not own, and never free the same pointer twice. When transferring ownership, set the original pointer to `NULL` immediately after the transfer. Callbacks and helper functions must not free buffers owned by their caller; on failure they should signal the error (e.g. return `0`/`NULL`/error code) and let the owning caller perform cleanup.
- Bounded string function safety: Always check the return value of `snprintf` (truncation if return `>= (int)sizeof(buffer)` or `< 0`) and never use the buffer if truncation occurred. Avoid `strncpy` and `strncat` — they do not guarantee NUL-termination and silently truncate; prefer `snprintf` or `memcpy` with an explicit length. After any bounded write, verify the buffer is properly NUL-terminated before use. On truncation or error, report to `stderr` and return an error code.
- Unsigned integer underflow safety: Never subtract from a `size_t` (or any unsigned type) without first guarding that the value is greater than zero. In loop bounds, prefer index-from-1 patterns (e.g. `for (size_t i = 1; i < count; ++i) { arr[i-1] = arr[i]; }`) over `count - 1` upper bounds. Before any expression of the form `n - k` where the type is unsigned, assert or guard that `n >= k`; if the guard fails, skip the operation or return an error.
- Portability: Never use `#define _POSIX_C_SOURCE`, `#define _DEFAULT_SOURCE`, `#define _GNU_SOURCE`, or any other feature-test macro. Code must compile clean under `cc -std=c99 -Wall -Wextra -pedantic` without any such defines. When a POSIX-only symbol (e.g. `kill`, `usleep`) is unavoidable, declare it explicitly with `extern` rather than enabling a feature-test macro.

### Testing Rules
- Every new `src/foo.c` must have a matching `tests/test_foo.c` unit test file, except `main.c`.
- Every new function or behavior change must add or update tests covering success, failure and edge cases (NULL, empty input, truncation, allocation-failure paths where feasible).
- Every bug fix must add a regression test that fails before the fix; known unfixed bugs must be marked `XFAIL`.
- User-visible or routing behavior changes must also be covered in `tests/e2e.sh`.
- `make test` must pass before a task is considered complete; never report work as done without running it.
- Tests must compile clean under `cc -std=c99 -Wall -Wextra -pedantic` and follow the same portability rules (no feature-test macros).
- The build system should enforce this rule (e.g., a make check that every `src/*.c` except `main.c` has a `tests/test_*.c`), mirroring the header rule.
