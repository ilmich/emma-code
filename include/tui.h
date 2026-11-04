#ifndef TUI_H
#define TUI_H

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
 * If the last message in the history is an assistant message, the text is
 * appended to its content; otherwise a new assistant message is created.
 * The screen is repainted after every call.
 */
void  tui_stream_append(tui_t *tui, const char *text);
/**
 * @brief Appends a streamed reasoning delta to the assistant message.
 *
 * Reasoning text is rendered as a dim block above the reply content. If
 * the last message in the history is not an assistant message, one is
 * created with empty content. The screen is repainted after every call.
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
void  tui_render(tui_t *tui);
char *tui_read_input(tui_t *tui);

#endif
