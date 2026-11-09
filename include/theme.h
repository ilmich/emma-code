#ifndef THEME_H
#define THEME_H

#include "toml.h"

/**
 * @file theme.h
 * @brief TUI color theme: built-in default plus optional [theme]
 *        overrides from config.toml.
 *
 * A theme owns dynamically allocated SGR escape sequences (e.g.
 * "\033[44m"). theme_default() fills the built-in palette, theme_load()
 * applies the optional [theme] section of a parsed config on top of it,
 * theme_copy() duplicates a theme and theme_free() releases it. Invalid
 * or missing values keep the corresponding default (a warning goes to
 * stderr); loading never fails the application.
 */

typedef struct {
    char *user_bg;      /**< SGR sequence selecting the user block background. */
    char *reasoning_on; /**< SGR sequence styling reasoning text (italic). */
    char *status;       /**< SGR sequence for the status line text. */
    char *input;        /**< SGR sequence for the live input box label. */
    char *tool;         /**< SGR sequence for tool block headers (marker+title). */
    char *tool_output;  /**< SGR sequence for tool output lines. */
    char *tool_error;   /**< SGR sequence for failed tool blocks. */
} theme_t;

/**
 * @brief Fills @p theme with the built-in default palette.
 *
 * On failure all fields are reset to NULL.
 *
 * @return 0 on success, -1 on NULL argument or allocation failure.
 */
int theme_default(theme_t *theme);

/**
 * @brief Applies the optional [theme] section of a parsed config.
 *
 * Accepted values: 16-color palette names ("black", "red", ..., plus
 * "bright_*"), numeric 256-color codes as foreground ("196") and
 * "on:<n>" as background ("on:17"). The reasoning key takes a style
 * name: "italic" (default), "dim" or "italic_dim". The tool keys
 * ("tool", "tool_output", "tool_error") take foreground colors.
 * Missing keys keep the current theme values; invalid values keep them
 * too (warning on stderr).
 *
 * @param theme Theme to update in place (must not be NULL).
 * @param doc   Parsed config root table (NULL is a no-op success).
 * @return 0 on success, -1 when theme is NULL.
 */
int theme_load(theme_t *theme, const toml_table_t *doc);

/**
 * @brief Duplicates @p src into @p dst (deep copy of every field).
 *
 * @param dst Receives the copy; on failure all its fields are NULL.
 * @param src Theme to copy (must not be NULL).
 * @return 0 on success, -1 on NULL argument or allocation failure.
 */
int theme_copy(theme_t *dst, const theme_t *src);

/**
 * @brief Releases all strings owned by @p theme and resets its fields
 *        to NULL. NULL-safe.
 */
void theme_free(theme_t *theme);

#endif /* THEME_H */
