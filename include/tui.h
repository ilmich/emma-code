#ifndef TUI_H
#define TUI_H

#include <stddef.h>

typedef struct tui_t tui_t;

int   tui_init(tui_t **tui);
void  tui_free(tui_t *tui);
void  tui_append_message(tui_t *tui, const char *role, const char *content);
/**
 * @brief Commits the final reply text: replaces the content of the last
 *        assistant message when one is in progress (built from streamed
 *        deltas), otherwise appends a new assistant message.
 *
 * @param tui TUI instance (must not be NULL)
 * @param content Final reply text (must not be NULL)
 * @param reasoning Final reasoning text shown as a dim block above the
 *                  reply (may be NULL or empty: no block is shown)
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
 * Reasoning text is rendered as a dim block above the reply content and
 * streamed into the terminal scrollback like content. If no streamed
 * assistant message is in progress, one is created with empty content.
 * Called for every delta.
 */
void  tui_stream_reasoning_append(tui_t *tui, const char *text);
void  tui_set_status(tui_t *tui, const char *status);
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
 * lines wrap at width columns; a newline always starts a new line. */

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

#endif
