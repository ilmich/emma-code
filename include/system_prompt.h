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

#endif // SYSTEM_PROMPT_H
