/**
 * @file system_prompt.h
 * @brief Built-in system prompt injected as the first session message.
 */

#ifndef SYSTEM_PROMPT_H
#define SYSTEM_PROMPT_H

/**
 * @brief Returns the built-in system prompt.
 *
 * The prompt is a compile-time constant: it is identical on every call
 * and for the whole lifetime of the process, so the first message of
 * every request payload is a stable, cache-friendly prefix. There is no
 * way to override it.
 *
 * @return NUL-terminated static string (never NULL, never empty).
 */
const char *system_prompt(void);

/**
 * @brief Returns the system prompt with the environment info block and
 *        the project instructions (./AGENTS.md, when present) appended.
 *
 * This is the text actually seeded as the first "system" message of a
 * session: static prompt + environment block (OS, cwd, local time) +
 * AGENTS.md block, computed once at startup and stable for the whole
 * session. If the environment info cannot be collected or AGENTS.md is
 * absent, the remaining blocks are still returned (with a warning on
 * stderr where appropriate), so session startup never fails because of
 * it.
 *
 * @return NUL-terminated heap string owned by the caller (free with
 *         free()), or NULL on allocation failure.
 */
char *system_prompt_full(void);

#endif // SYSTEM_PROMPT_H
