#ifndef TUI_H
#define TUI_H

#include <stddef.h>

#include "theme.h"

typedef struct tui_t tui_t;

/**
 * @brief Initializes the TUI with the given color theme.
 *
 * @param tui   Receives the TUI instance on success.
 * @param theme Theme to paint with (copied; NULL selects the built-in
 *              default theme).
 * @return 0 on success, -1 on failure (no allocation returned).
 */
int   tui_init(tui_t **tui, const theme_t *theme);
void  tui_free(tui_t *tui);
void  tui_append_message(tui_t *tui, const char *role, const char *content);
/**
 * @brief Commits the final reply text: replaces the content of the last
 *        assistant message when one is in progress (built from streamed
 *        deltas), otherwise appends a new assistant message.
 *
 * @param tui TUI instance (must not be NULL)
 * @param content Final reply text (must not be NULL)
 * @param reasoning Final reasoning text shown as an italic block above
 *                  the reply (may be NULL or empty: no block is shown)
 */
void  tui_replace_assistant_message(tui_t *tui, const char *content,
                                    const char *reasoning);
/**
 * @brief Appends a streamed text delta to the assistant message.
 *
 * If a streamed assistant message is in progress, the text is appended to
 * its content; otherwise a new assistant message is created. Completed
 * lines of the message are printed into the terminal scrollback; the
 * trailing (still growing) line stays in the live area above the input
 * box. Called for every delta.
 */
void  tui_stream_append(tui_t *tui, const char *text);
/**
 * @brief Appends a streamed reasoning delta to the assistant message.
 *
 * Reasoning text is rendered as an italic block above the reply content
 * and streamed into the terminal scrollback like content. If no streamed
 * assistant message is in progress, one is created with empty content.
 * Called for every delta.
 */
void  tui_stream_reasoning_append(tui_t *tui, const char *text);
void  tui_set_status(tui_t *tui, const char *status);
/**
 * @brief Starts a tool block in the transcript: prints the styled header
 *        line (colored marker + title) and starts the status-line
 *        spinner ("⠋ running <title>…") until tui_tool_finish() is
 *        called for the same block.
 *
 * @param tui   TUI instance (may be NULL: no-op)
 * @param title One-line block title (must not be NULL or empty; no-op
 *              otherwise)
 */
void  tui_tool_start(tui_t *tui, const char *title);
/**
 * @brief Completes the tool block started by tui_tool_start(): stops
 *        the spinner and prints the full tool output below the header,
 *        styled as output on success or as an error block (✗ marker
 *        color) on failure.
 *
 * @param tui    TUI instance (may be NULL: no-op)
 * @param ok     Nonzero when the tool succeeded, 0 on failure
 * @param output Full tool output to display (may be NULL or empty: only
 *               the block is closed)
 */
void  tui_tool_finish(tui_t *tui, int ok, const char *output);
/**
 * @brief Asks the user for a yes/no confirmation with an inline prompt.
 *
 * Prints "<prompt> [y/n]" on a new line and blocks until the user presses
 * y/Y (approved) or n/N, Ctrl-C or EOF (denied). Any other key is ignored.
 *
 * @param tui TUI instance (must not be NULL)
 * @param prompt Question shown to the user (must not be NULL)
 * @return 1 when approved, 0 when denied or on invalid arguments
 */
int   tui_confirm(tui_t *tui, const char *prompt);
/**
 * @brief Repaints the live area (status/separator row and input box).
 *
 * In transcript mode the conversation history lives in the terminal's
 * own scrollback; only the live area at the bottom of the screen is
 * erased and redrawn here.
 */
void  tui_render(tui_t *tui);
char *tui_read_input(tui_t *tui);

/* Geometry helpers shared by the TUI and exported for unit tests. They
 * implement the input box wrapping rules: the first line is prefixed by
 * the 5-column "You: " label (wrapping at first_width columns), later
 * lines wrap at width columns; a newline always starts a new line.
 * Transcript messages carry no role labels: user messages are rendered
 * as full-width colored background blocks (1 column of padding per
 * side), other roles as plain text indented by the same 1 column. */

/**
 * @brief Computes the logical line and column of a byte index.
 *
 * @param buf        Input buffer (may be NULL only when len is 0)
 * @param len        Buffer length in bytes
 * @param index      Byte index to map (clamped to len)
 * @param first_width Wrapping width of the first line
 * @param width      Wrapping width of the following lines
 * @param out_line   Receives the 0-based visual line (may be NULL)
 * @param out_col    Receives the 0-based column (may be NULL)
 */
void  tui_input_line_col(const char *buf, size_t len, size_t index,
                         int first_width, int width, int *out_line,
                         int *out_col);
/**
 * @brief Maps a visual line/column back to a byte index.
 *
 * The column is clamped to the target line's end (a newline or a wrap
 * boundary).
 *
 * @return 0 on success (with *out_index set), -1 when line is beyond
 *         the input content or buf is NULL.
 */
int   tui_input_index_at(const char *buf, size_t len, int first_width,
                         int width, int line, int col, size_t *out_index);
/**
 * @brief Last display line index (inclusive) to emit to the transcript
 *        for a streamed message.
 *
 * Without flush_all the trailing (still growing) line is held back in
 * the live area, so at most lines 0..total-2 are emitted.
 *
 * @return The last line index to emit, or -1 when nothing to emit.
 */
int   tui_stream_emit_to(int total_lines, int flush_all);

/**
 * @brief Builds the startup banner lines (block-letter art spelling
 *        "EMMA-CODE" plus the trailing version line "v" EMMA_VERSION).
 *
 * The lines are plain text (no SGR sequences): the caller decides how
 * to style them. Every line fits an 80-column terminal.
 *
 * @param out_count Receives the number of returned lines (may be NULL;
 *                  set to 0 on failure).
 * @return A malloc'd array of malloc'd lines owned by the caller
 *         (free each entry and the array itself), or NULL on
 *         allocation failure (*out_count is 0 in that case).
 */
char **tui_banner_lines(size_t *out_count);

/**
 * @brief Prints the startup banner into the transcript scrollback.
 *
 * Writes the banner lines (from tui_banner_lines()) without wrapping,
 * followed by a blank line, then repaints the live area. The version
 * line is styled with the theme's status color. A NULL @p tui is a
 * no-op; allocation failures skip the banner without side effects.
 *
 * @param tui TUI instance (may be NULL)
 */
void  tui_show_banner(tui_t *tui);

/**
 * Clears the visible screen and restarts the transcript from scratch:
 * drops every stored message and streaming state, moves the cursor home,
 * clears the screen (the terminal's own scrollback above stays reachable
 * by scrolling up) and prints the startup banner again.
 */
void  tui_clear(tui_t *tui);

#endif
