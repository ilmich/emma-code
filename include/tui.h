#ifndef TUI_H
#define TUI_H

typedef struct tui_t tui_t;

int   tui_init(tui_t **tui);
void  tui_free(tui_t *tui);
void  tui_append_message(tui_t *tui, const char *role, const char *content);
void  tui_set_status(tui_t *tui, const char *status);
void  tui_render(tui_t *tui);
char *tui_read_input(tui_t *tui);

#endif
