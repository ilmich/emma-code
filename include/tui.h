#ifndef TUI_H
#define TUI_H

typedef struct tui_t tui_t;

int   tui_init(tui_t **tui);
void  tui_free(tui_t *tui);
void  tui_append_message(tui_t *tui, const char *role, const char *content);
/**
 * @brief Appends a streamed text delta to the assistant message.
 *
 * If the last message in the history is an assistant message, the text is
 * appended to its content; otherwise a new assistant message is created.
 * The screen is repainted after every call.
 */
void  tui_stream_append(tui_t *tui, const char *text);
void  tui_set_status(tui_t *tui, const char *status);
void  tui_render(tui_t *tui);
char *tui_read_input(tui_t *tui);

#endif
