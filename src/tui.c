/* POSIX symbols not declared by strict ISO C99 headers on this toolchain
 * are declared explicitly here instead of enabling feature-test macros
 * (see AGENTS.md portability rules). */
extern char *strdup(const char *s);
extern int pipe(int pipefd[2]);
extern int close(int fd);
#ifndef SIGWINCH
#define SIGWINCH 28
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <signal.h>
#include "tui.h"
#include "markdown.h"

#define INPUT_MAX_LINES    5
#define INPUT_BUF_INIT     256
#define MESSAGES_INIT      16
#define SEPARATOR_CHAR     '-'
#define OUT_BUF_SIZE       8192

#define ANSI_RESET         "\033[0m"
#define ANSI_DIM           "\033[0;2m"
#define ANSI_BOLD_GREEN    "\033[1;32m"
#define ANSI_BOLD_BLUE     "\033[1;34m"
#define ANSI_BOLD_YELLOW   "\033[1;33m"
#define ANSI_CLEAR_SCREEN  "\033[2J"
#define ANSI_CURSOR_HOME   "\033[H"
#define ANSI_CLEAR_LINE    "\033[K"
#define ANSI_HIDE_CURSOR   "\033[?25l"
#define ANSI_SHOW_CURSOR   "\033[?25h"

typedef struct {
    char *role;
    char *content;
    char *reasoning;
} tui_message_t;

struct tui_t {
    struct termios orig_termios;
    int            raw_mode;

    int            term_rows;
    int            term_cols;

    tui_message_t *messages;
    size_t         msg_count;
    size_t         msg_cap;

    char          *status;

    char          *input_buf;
    size_t         input_len;
    size_t         input_cap;
    size_t         input_cursor;

    int            scroll_offset;
};

static tui_t *g_tui = NULL;

// Set by the SIGWINCH handler; processed in the main thread (signal handlers
// must only touch async-signal-safe state).
static volatile sig_atomic_t g_resize_pending = 0;

// Self-pipe used to wake select() when SIGWINCH arrives: the handler writes
// one byte so a resize can never be lost while read_byte() sleeps.
static int g_resize_pipe[2] = { -1, -1 };

static void out_flush(void); // defined below; used by the signal handler
static int read_byte(unsigned char *b); // defined below; used by tui_confirm

static void tui_get_term_size(tui_t *tui) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        tui->term_rows = ws.ws_row;
        tui->term_cols = ws.ws_col;
    } else {
        tui->term_rows = 24;
        tui->term_cols = 80;
    }
}

static void sigwinch_handler(int sig) {
    ssize_t ignored;
    (void)sig;
    g_resize_pending = 1;
    // write() is async-signal-safe; leftover bytes are drained in read_byte().
    if (g_resize_pipe[1] >= 0) {
        ignored = write(g_resize_pipe[1], "r", 1);
        (void)ignored;
    }
}

// Restore the terminal before dying on a fatal signal so the user's shell is
// not left in raw mode with a hidden cursor.
static void fatal_signal_handler(int sig) {
    if (g_tui) {
        out_flush(); // write() is async-signal-safe
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_tui->orig_termios);
        g_tui->raw_mode = 0;
        (void)!write(STDOUT_FILENO, ANSI_SHOW_CURSOR, sizeof(ANSI_SHOW_CURSOR) - 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static int tui_enter_raw(tui_t *tui) {
    if (tcgetattr(STDIN_FILENO, &tui->orig_termios) != 0) {
        return -1;
    }
    struct termios raw = tui->orig_termios;
    raw.c_iflag &= ~(unsigned)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cflag |= (unsigned)(CS8);
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        return -1;
    }
    tui->raw_mode = 1;
    return 0;
}

static void tui_exit_raw(tui_t *tui) {
    if (tui->raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &tui->orig_termios);
        tui->raw_mode = 0;
    }
}

// Buffered terminal output: batches rendering into large write() calls
// instead of one syscall per character.
static char   out_buf[OUT_BUF_SIZE];
static size_t out_len = 0;

static void out_flush(void) {
    size_t off = 0;
    while (off < out_len) {
        ssize_t n = write(STDOUT_FILENO, out_buf + off, out_len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break; // Terminal is gone; drop the rest of the buffer
        }
        off += (size_t)n;
    }
    out_len = 0;
}

static void out_write(const char *s, size_t len) {
    if (len > OUT_BUF_SIZE) {
        // Larger than the buffer itself: flush pending data, write directly.
        out_flush();
        size_t off = 0;
        while (off < len) {
            ssize_t n = write(STDOUT_FILENO, s + off, len - off);
            if (n < 0) {
                if (errno == EINTR) continue;
                return;
            }
            off += (size_t)n;
        }
        return;
    }
    if (out_len + len > OUT_BUF_SIZE) {
        out_flush();
    }
    memcpy(out_buf + out_len, s, len);
    out_len += len;
}

static void cursor_move(int row, int col) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\033[%d;%dH", row, col);
    if (n > 0 && n < (int)sizeof(buf)) {
        out_write(buf, (size_t)n);
    }
}

static void write_str(const char *s) {
    out_write(s, strlen(s));
}

// Word-wraps plain reasoning text into dim-styled lines owned by the
// caller (free with md_lines_free()). Returns NULL when there is nothing
// to render. Newlines force a line break; words longer than the width
// are hard-broken at the wrapping width.
static md_line_t *reasoning_text_lines(const char *text, int width,
                                       size_t *out_count) {
    const size_t dim_len = sizeof(ANSI_DIM) - 1;
    const size_t reset_len = sizeof(ANSI_RESET) - 1;
    size_t cap = 8;
    size_t count = 0;
    md_line_t *lines = NULL;
    size_t i = 0;
    size_t text_len;

    if (out_count) *out_count = 0;
    if (text == NULL || text[0] == '\0' || width < 1) return NULL;
    text_len = strlen(text);

    lines = malloc(cap * sizeof(md_line_t));
    if (!lines) return NULL;

    while (i < text_len) {
        size_t take = 0;
        size_t last_space = 0;
        int has_space = 0;
        int forced_break = 0;
        size_t start, seg_len;
        char *row;

        if (count >= cap) {
            md_line_t *tmp;
            cap *= 2;
            tmp = realloc(lines, cap * sizeof(md_line_t));
            if (!tmp) {
                md_lines_free(lines, count);
                return NULL;
            }
            lines = tmp;
        }

        /* Greedy wrap: take up to width chars, prefer breaking at the
         * last space inside the segment; a newline forces the break. */
        while (i + take < text_len && (int)take < width) {
            char c = text[i + take];
            if (c == '\n') {
                forced_break = 1;
                break;
            }
            if (c == ' ') {
                last_space = take;
                has_space = 1;
            }
            take++;
        }
        if (!forced_break && i + take < text_len && has_space &&
            last_space > 0) {
            take = last_space; /* break at the last space */
        }

        start = i;
        seg_len = take;
        if (seg_len > 0 && text[start] == ' ') {
            start++; /* drop the space carried over from the wrap */
            seg_len--;
        }

        row = malloc(dim_len + seg_len + reset_len + 1);
        if (!row) {
            md_lines_free(lines, count);
            return NULL;
        }
        memcpy(row, ANSI_DIM, dim_len);
        memcpy(row + dim_len, text + start, seg_len);
        memcpy(row + dim_len + seg_len, ANSI_RESET, reset_len + 1);
        lines[count].text = row;
        count++;

        if (forced_break) {
            i += take + 1; /* consume the newline too */
        } else {
            i += take;
            while (i < text_len && text[i] == ' ') i++; /* wrap spaces */
        }
    }

    if (out_count) *out_count = count;
    return lines;
}

// Number of pre-wrapped markdown lines a message renders to. Empty or
// NULL content still occupies one (label) row.
static int count_lines_for_content(const char *content, int width) {
    size_t count = 0;
    md_line_t *lines = md_render(content, width, &count);
    md_lines_free(lines, count);
    return (count > 0) ? (int)count : 1;
}

// Number of rows the reasoning block of a message renders to: one dim
// label row plus the wrapped dim reasoning text. Zero when the message
// carries no reasoning.
static int count_lines_for_reasoning(const char *reasoning, int width) {
    size_t count = 0;
    md_line_t *lines;

    if (reasoning == NULL || reasoning[0] == '\0') return 0;
    lines = reasoning_text_lines(reasoning, width, &count);
    md_lines_free(lines, count);
    return (int)count + 1; /* + label row */
}

static int count_input_lines(tui_t *tui) {
    int label_len = 5; /* "You: " is 5 columns */
    int first_line_width = tui->term_cols - label_len;
    if (first_line_width < 1) first_line_width = 1;

    if (tui->input_len == 0) return 1;

    int lines = 1;
    int col = 0;
    int first_line = 1;
    int effective_width = first_line_width;

    for (size_t i = 0; i < tui->input_len; i++) {
        char c = tui->input_buf[i];
        if (c == '\n') {
            lines++;
            col = 0;
            first_line = 0;
            effective_width = tui->term_cols;
        } else {
            col++;
            if (col >= effective_width) {
                lines++;
                col = 0;
                if (first_line) {
                    first_line = 0;
                    effective_width = tui->term_cols;
                }
            }
        }
    }
    return lines;
}

static int compute_input_box_height(tui_t *tui) {
    int lines = count_input_lines(tui);
    if (lines < 1) lines = 1;
    if (lines > INPUT_MAX_LINES) lines = INPUT_MAX_LINES;
    return lines;
}

static int compute_history_rows(tui_t *tui) {
    int input_h = compute_input_box_height(tui);
    int history_rows = tui->term_rows - input_h - 1;
    if (history_rows < 1) history_rows = 1;
    return history_rows;
}

static int total_rendered_lines(tui_t *tui) {
    int total = 0;
    int role_prefix = 12;
    int content_width = tui->term_cols - role_prefix;
    if (content_width < 1) content_width = 1;
    for (size_t i = 0; i < tui->msg_count; i++) {
        total += count_lines_for_reasoning(tui->messages[i].reasoning,
                                           content_width);
        total += count_lines_for_content(tui->messages[i].content, content_width);
    }
    return total;
}

static void render_history(tui_t *tui) {
    int history_rows = compute_history_rows(tui);
    int role_prefix  = 12;
    int content_width = tui->term_cols - role_prefix;
    if (content_width < 1) content_width = 1;

    int total = total_rendered_lines(tui);
    int max_scroll = total - history_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (tui->scroll_offset > max_scroll) tui->scroll_offset = max_scroll;

    int lines_to_skip = tui->scroll_offset;
    int screen_row = 1;

    for (size_t i = 0; i < tui->msg_count && screen_row <= history_rows; i++) {
        const char *role    = tui->messages[i].role;
        const char *content = tui->messages[i].content;
        const char *reasoning = tui->messages[i].reasoning;

        // Reasoning block: one dim label row plus the wrapped dim text.
        size_t r_count = 0;
        md_line_t *r_lines = reasoning_text_lines(reasoning, content_width,
                                                  &r_count);
        int r_rows = (r_count > 0) ? (int)r_count + 1 : 0;

        size_t md_count = 0;
        int msg_lines = r_rows +
            count_lines_for_content(content, content_width);

        if (lines_to_skip >= msg_lines) {
            lines_to_skip -= msg_lines;
            md_lines_free(r_lines, r_count);
            continue;
        }

        const char *color = ANSI_BOLD_GREEN;
        const char *label = "     User";
        if (strcmp(role, "assistant") == 0) {
            color = ANSI_BOLD_BLUE;
            label = "Assistant";
        } else if (strcmp(role, "system") == 0) {
            color = ANSI_BOLD_YELLOW;
            label = "  System";
        }

        // Render the message content into pre-wrapped, styled markdown
        // lines. An empty message still shows its label on one row.
        md_line_t *lines = md_render(content, content_width, &md_count);

        int line_in_msg = lines_to_skip; // first visible message line
        lines_to_skip = 0;               // consumed by this message

        cursor_move(screen_row, 1);
        write_str(ANSI_CLEAR_LINE);

        // Always label the first visible row of the message, even when the
        // upper part of the message is scrolled away.
        write_str(color);
        write_str(label);
        write_str(": ");
        write_str(ANSI_RESET);

        for (int li = line_in_msg;
             li < msg_lines && screen_row <= history_rows; li++) {
            if (li > line_in_msg) {
                // Erase stale characters from a previous frame on this row.
                write_str(ANSI_CLEAR_LINE);
                screen_row++;
                if (screen_row <= history_rows) {
                    cursor_move(screen_row, 1);
                    write_str(ANSI_CLEAR_LINE);
                }
            }
            if (li < r_rows) {
                // Reasoning block: first row is the dim label, the rest
                // are the pre-styled dim reasoning lines.
                if (li == 0) {
                    write_str(ANSI_DIM);
                    write_str("reasoning");
                    write_str(ANSI_RESET);
                } else if (r_lines[li - 1].text) {
                    write_str(r_lines[li - 1].text);
                }
            } else if ((size_t)(li - r_rows) < md_count &&
                       lines[li - r_rows].text) {
                write_str(lines[li - r_rows].text);
            }
        }

        // Clear from here so leftovers from a previous frame are erased.
        write_str(ANSI_CLEAR_LINE);

        screen_row++;
        md_lines_free(r_lines, r_count);
        md_lines_free(lines, md_count);
    }

    while (screen_row <= history_rows) {
        cursor_move(screen_row, 1);
        write_str(ANSI_CLEAR_LINE);
        screen_row++;
    }
}

static void render_separator(tui_t *tui) {
    int sep_row = compute_history_rows(tui) + 1;
    cursor_move(sep_row, 1);
    write_str(ANSI_CLEAR_LINE);

    if (tui->status && tui->status[0]) {
        write_str(ANSI_BOLD_YELLOW);
        write_str(" ");
        write_str(tui->status);
        write_str(ANSI_RESET);
        int used = 1 + (int)strlen(tui->status);
        int remaining = tui->term_cols - used;
        for (int i = 0; i < remaining; i++) {
            write_str(" ");
        }
    } else {
        for (int i = 0; i < tui->term_cols; i++) {
            char ch[2] = {SEPARATOR_CHAR, 0};
            write_str(ch);
        }
    }
}

static void render_input(tui_t *tui) {
    int history_rows = compute_history_rows(tui);
    int input_h      = compute_input_box_height(tui);
    int input_start  = history_rows + 2;
    int label_len    = 5; /* "You: " is 5 columns */
    int first_width  = tui->term_cols - label_len;
    if (first_width < 1) first_width = 1;

    for (int r = input_start; r < input_start + input_h; r++) {
        cursor_move(r, 1);
        write_str(ANSI_CLEAR_LINE);
    }

    // Logical line the cursor sits on, using the same wrapping rules as
    // count_input_lines(), so it can be kept inside the visible box.
    int cursor_line = 0;
    {
        int col = 0;
        int first_line = 1;
        int effective_width = first_width;
        for (size_t i = 0; i < tui->input_cursor && i < tui->input_len; i++) {
            char c = tui->input_buf[i];
            if (c == '\n') {
                cursor_line++;
                col = 0;
                first_line = 0;
                effective_width = tui->term_cols;
            } else {
                col++;
                if (col >= effective_width) {
                    cursor_line++;
                    col = 0;
                    if (first_line) {
                        first_line = 0;
                        effective_width = tui->term_cols;
                    }
                }
            }
        }
    }

    // If the text does not fit into the box, scroll it so that the line the
    // cursor is on stays visible.
    int start_line = cursor_line - (input_h - 1);
    if (start_line < 0) start_line = 0;

    cursor_move(input_start, 1);
    int drawn_row = -1;
    if (start_line == 0) {
        write_str(ANSI_BOLD_GREEN);
        write_str("You: ");
        write_str(ANSI_RESET);
        drawn_row = input_start; // Label is already drawn on this row
    }

    int cursor_row = input_start;
    int cursor_col = 1;
    int line_idx   = 0;
    int col        = 0;
    int first_line = 1;
    int effective_width = first_width;

    for (size_t i = 0; i <= tui->input_len; i++) {
        if (i == tui->input_cursor) {
            cursor_row = input_start + (line_idx - start_line);
            cursor_col = col + 1;
            if (line_idx == 0 && start_line == 0) cursor_col += label_len;
        }
        if (i == tui->input_len) break;

        char c = tui->input_buf[i];

        if (c == '\n') {
            line_idx++;
            col = 0;
            first_line = 0;
            effective_width = tui->term_cols;
            if (line_idx - start_line >= input_h) break;
            continue;
        }

        int rel = line_idx - start_line;
        if (rel >= input_h) break;

        if (rel >= 0) {
            int row = input_start + rel;
            if (drawn_row != row) {
                cursor_move(row, 1);
                drawn_row = row;
            }
            char ch[2] = {c, 0};
            write_str(ch);
        }

        col++;
        if (col >= effective_width) {
            col = 0;
            line_idx++;
            if (first_line) {
                first_line = 0;
                effective_width = tui->term_cols;
            }
            if (line_idx - start_line >= input_h) break;
        }
    }

    // Belt and braces: never leave the cursor outside the input box.
    if (cursor_row > input_start + input_h - 1) cursor_row = input_start + input_h - 1;
    if (cursor_row < input_start) cursor_row = input_start;
    cursor_move(cursor_row, cursor_col);
}

void tui_render(tui_t *tui) {
    write_str(ANSI_HIDE_CURSOR);
    render_history(tui);
    render_separator(tui);
    render_input(tui);
    write_str(ANSI_SHOW_CURSOR);
    out_flush();
}

int tui_init(tui_t **out) {
    tui_t *tui = calloc(1, sizeof(tui_t));
    int rc = -1;

    if (!tui) {
        fprintf(stderr, "tui_init: calloc failed\n");
        return -1; /* nothing to clean up yet */
    }

    tui->messages = malloc(MESSAGES_INIT * sizeof(tui_message_t));
    if (!tui->messages) {
        fprintf(stderr, "tui_init: malloc messages failed\n");
        goto cleanup;
    }
    tui->msg_cap = MESSAGES_INIT;

    tui->input_cap = INPUT_BUF_INIT;
    tui->input_buf = malloc(tui->input_cap);
    if (!tui->input_buf) {
        fprintf(stderr, "tui_init: malloc input_buf failed\n");
        goto cleanup;
    }
    tui->input_buf[0] = '\0';

    tui_get_term_size(tui);

    if (tui_enter_raw(tui) != 0) {
        fprintf(stderr, "tui_init: failed to enter raw mode\n");
        goto cleanup;
    }

    g_tui = tui;

    if (pipe(g_resize_pipe) != 0) {
        // Degraded mode: resizes are still detected on the next keypress.
        fprintf(stderr, "tui_init: pipe failed, resize wakeups disabled\n");
        g_resize_pipe[0] = -1;
        g_resize_pipe[1] = -1;
    }

    signal(SIGWINCH, sigwinch_handler);
    signal(SIGINT, fatal_signal_handler);
    signal(SIGTERM, fatal_signal_handler);

    write_str(ANSI_CLEAR_SCREEN);
    write_str(ANSI_CURSOR_HOME);

    *out = tui;
    tui = NULL; /* ownership transferred to the caller */
    rc = 0;

cleanup:
    if (tui != NULL) {
        free(tui->input_buf);
        free(tui->messages);
        free(tui);
    }
    return rc;
}

void tui_free(tui_t *tui) {
    if (!tui) return;

    tui_exit_raw(tui);
    write_str(ANSI_CLEAR_SCREEN);
    write_str(ANSI_CURSOR_HOME);
    write_str(ANSI_SHOW_CURSOR);
    out_flush();

    for (size_t i = 0; i < tui->msg_count; i++) {
        free(tui->messages[i].role);
        free(tui->messages[i].content);
        free(tui->messages[i].reasoning);
    }
    free(tui->messages);
    free(tui->input_buf);
    free(tui->status);

    if (g_resize_pipe[0] >= 0) {
        close(g_resize_pipe[0]);
        g_resize_pipe[0] = -1;
    }
    if (g_resize_pipe[1] >= 0) {
        close(g_resize_pipe[1]);
        g_resize_pipe[1] = -1;
    }

    if (g_tui == tui) {
        g_tui = NULL;
        signal(SIGWINCH, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
    }

    free(tui);
}

// Adjusts the scroll offset after the message history changes, then
// repaints the screen.
static void tui_after_history_change(tui_t *tui) {
    int role_prefix   = 12;
    int content_width = tui->term_cols - role_prefix;
    if (content_width < 1) content_width = 1;
    int history_rows  = compute_history_rows(tui);
    int total         = total_rendered_lines(tui);
    int overflow      = total - history_rows;
    if (overflow > 0) {
        tui->scroll_offset = overflow;
    }

    tui_render(tui);
}

void tui_append_message(tui_t *tui, const char *role, const char *content) {
    if (!tui || !role || !content) return;

    if (tui->msg_count >= tui->msg_cap) {
        size_t new_cap = tui->msg_cap * 2;
        tui_message_t *tmp = realloc(tui->messages, new_cap * sizeof(tui_message_t));
        if (!tmp) {
            fprintf(stderr, "tui_append_message: realloc failed\n");
            return;
        }
        tui->messages = tmp;
        tui->msg_cap  = new_cap;
    }

    char *r = strdup(role);
    char *c = strdup(content);
    if (!r || !c) {
        fprintf(stderr, "tui_append_message: strdup failed\n");
        free(r);
        free(c);
        return;
    }

    tui->messages[tui->msg_count].role    = r;
    tui->messages[tui->msg_count].content = c;
    tui->messages[tui->msg_count].reasoning = NULL;
    tui->msg_count++;

    tui_after_history_change(tui);
}

void tui_stream_reasoning_append(tui_t *tui, const char *text) {
    if (!tui || !text || !*text) return;

    if (tui->msg_count == 0 || tui->messages[tui->msg_count - 1].role == NULL ||
        strcmp(tui->messages[tui->msg_count - 1].role, "assistant") != 0) {
        // No assistant message yet: the first reasoning delta starts one
        // (content stays empty until the first content delta arrives).
        tui_append_message(tui, "assistant", "");
        if (tui->msg_count == 0 ||
            strcmp(tui->messages[tui->msg_count - 1].role, "assistant") != 0) {
            return; // append failed (allocation error already reported)
        }
    }

    {
        tui_message_t *m = &tui->messages[tui->msg_count - 1];
        size_t rlen = m->reasoning ? strlen(m->reasoning) : 0;
        size_t tlen = strlen(text);
        char *tmp = realloc(m->reasoning, rlen + tlen + 1);
        if (!tmp) {
            fprintf(stderr, "tui_stream_reasoning_append: realloc failed\n");
            return;
        }
        m->reasoning = tmp;
        memcpy(m->reasoning + rlen, text, tlen + 1);
    }

    tui_after_history_change(tui);
}

void tui_stream_append(tui_t *tui, const char *text) {
    if (!tui || !text || !*text) return;

    if (tui->msg_count > 0 && tui->messages[tui->msg_count - 1].role != NULL &&
        strcmp(tui->messages[tui->msg_count - 1].role, "assistant") == 0) {
        // Extend the in-progress assistant message with the delta.
        tui_message_t *m = &tui->messages[tui->msg_count - 1];
        size_t clen = strlen(m->content);
        size_t tlen = strlen(text);
        char *tmp = realloc(m->content, clen + tlen + 1);
        if (!tmp) {
            fprintf(stderr, "tui_stream_append: realloc failed\n");
            return;
        }
        m->content = tmp;
        memcpy(m->content + clen, text, tlen + 1);
    } else {
        // No assistant message yet: the first delta starts one.
        tui_append_message(tui, "assistant", text);
        return; // tui_append_message already rendered
    }

    tui_after_history_change(tui);
}

void tui_replace_assistant_message(tui_t *tui, const char *content,
                                   const char *reasoning) {
    if (!tui || !content) return;

    if (tui->msg_count > 0 && tui->messages[tui->msg_count - 1].role != NULL &&
        strcmp(tui->messages[tui->msg_count - 1].role, "assistant") == 0) {
        // Commit the final reply into the message the streamed deltas
        // have already created, so the text is not shown twice.
        tui_message_t *m = &tui->messages[tui->msg_count - 1];
        char *c = strdup(content);
        char *r = NULL;
        if (c == NULL) {
            fprintf(stderr, "tui_replace_assistant_message: strdup failed\n");
            return;
        }
        if (reasoning != NULL && reasoning[0] != '\0') {
            r = strdup(reasoning);
            if (r == NULL) {
                fprintf(stderr,
                        "tui_replace_assistant_message: strdup failed\n");
                free(c);
                return;
            }
        }
        free(m->content);
        m->content = c;
        free(m->reasoning);
        m->reasoning = r;
        tui_after_history_change(tui);
        return;
    }

    // No streamed assistant message in progress: append normally.
    tui_append_message(tui, "assistant", content);
    if (tui->msg_count > 0 &&
        tui->messages[tui->msg_count - 1].role != NULL &&
        strcmp(tui->messages[tui->msg_count - 1].role, "assistant") == 0 &&
        reasoning != NULL && reasoning[0] != '\0') {
        tui_message_t *m = &tui->messages[tui->msg_count - 1];
        m->reasoning = strdup(reasoning);
        if (m->reasoning == NULL) {
            fprintf(stderr, "tui_replace_assistant_message: strdup failed\n");
        }
    }
}

void tui_set_status(tui_t *tui, const char *status) {
    if (!tui) return;
    free(tui->status);
    tui->status = NULL;
    if (status) {
        tui->status = strdup(status);
        if (!tui->status) {
            fprintf(stderr, "tui_set_status: strdup failed\n");
        }
    }
    render_separator(tui);
    render_input(tui);
    out_flush();
}

int tui_confirm(tui_t *tui, const char *prompt) {
    if (!tui || !prompt) return 0;

    char line[256];
    int n = snprintf(line, sizeof(line), "\r\n%s [y/n] ", prompt);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        write_str("\r\n[confirm prompt truncated] [y/n] ");
    } else {
        write_str(line);
    }
    out_flush();

    while (1) {
        unsigned char b;
        if (read_byte(&b) != 0) {
            write_str("n\r\n");
            out_flush();
            return 0; /* EOF/terminal gone: deny */
        }
        if (b == 'y' || b == 'Y') {
            write_str("y\r\n");
            out_flush();
            return 1;
        }
        if (b == 'n' || b == 'N') {
            write_str("n\r\n");
            out_flush();
            return 0;
        }
        if (b == 0x03) {
            /* Ctrl-C: deny like every other escape path. */
            write_str("n\r\n");
            out_flush();
            return 0;
        }
        /* Any other key: keep waiting. */
    }
}

static int input_ensure_cap(tui_t *tui, size_t needed) {
    if (needed < tui->input_cap) return 0;
    size_t new_cap = tui->input_cap * 2;
    while (new_cap <= needed) new_cap *= 2;
    char *tmp = realloc(tui->input_buf, new_cap);
    if (!tmp) {
        fprintf(stderr, "input_ensure_cap: realloc failed\n");
        return -1;
    }
    tui->input_buf = tmp;
    tui->input_cap = new_cap;
    return 0;
}

static void input_insert(tui_t *tui, char c) {
    if (input_ensure_cap(tui, tui->input_len + 2) != 0) return;
    memmove(tui->input_buf + tui->input_cursor + 1,
            tui->input_buf + tui->input_cursor,
            tui->input_len - tui->input_cursor + 1);
    tui->input_buf[tui->input_cursor] = c;
    tui->input_len++;
    tui->input_cursor++;
}

static void input_delete_before(tui_t *tui) {
    if (tui->input_cursor == 0) return;
    memmove(tui->input_buf + tui->input_cursor - 1,
            tui->input_buf + tui->input_cursor,
            tui->input_len - tui->input_cursor + 1);
    tui->input_len--;
    tui->input_cursor--;
}

static void input_delete_after(tui_t *tui) {
    if (tui->input_cursor >= tui->input_len) return;
    memmove(tui->input_buf + tui->input_cursor,
            tui->input_buf + tui->input_cursor + 1,
            tui->input_len - tui->input_cursor);
    tui->input_len--;
}

static void input_clear(tui_t *tui) {
    tui->input_len    = 0;
    tui->input_cursor = 0;
    tui->input_buf[0] = '\0';
}

static int read_byte(unsigned char *b) {
    for (;;) {
        // Handle pending resizes here (in the main thread) and retry instead
        // of treating them as EOF.
        if (g_resize_pending) {
            g_resize_pending = 0;
            tui_get_term_size(g_tui);
            tui_render(g_tui);
        }

        // Make sure the screen is fully painted before going to sleep.
        out_flush();

        // Wait for stdin OR the resize self-pipe: a SIGWINCH arriving after
        // the flag check above wakes the select via the pipe, so it can
        // never be lost while we sleep.
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = STDIN_FILENO;
        if (g_resize_pipe[0] >= 0) {
            FD_SET(g_resize_pipe[0], &rfds);
            if (g_resize_pipe[0] > maxfd) maxfd = g_resize_pipe[0];
        }
        int pret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (pret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        if (g_resize_pipe[0] >= 0 && FD_ISSET(g_resize_pipe[0], &rfds)) {
            // Drain the wakeup bytes, then re-check the pending flag.
            char drain[256];
            (void)read(g_resize_pipe[0], drain, sizeof(drain));
            continue;
        }
        if (pret == 0 || !FD_ISSET(STDIN_FILENO, &rfds)) {
            continue;
        }

        ssize_t n = read(STDIN_FILENO, b, 1);
        if (n == 1) return 0;
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
}

char *tui_read_input(tui_t *tui) {
    if (g_resize_pending) {
        g_resize_pending = 0;
        tui_get_term_size(tui);
    }
    input_clear(tui);
    tui_render(tui);

    while (1) {
        unsigned char b;
        if (read_byte(&b) != 0) return NULL;

        if (b == 0x03) {
            return NULL;
        }

        if (b == '\r') {
            if (tui->input_len == 0) continue;
            char *result = strdup(tui->input_buf);
            if (!result) {
                fprintf(stderr, "tui_read_input: strdup failed\n");
                return NULL;
            }
            input_clear(tui);
            return result;
        }

        if (b == '\n') {
            input_insert(tui, '\n');
            render_separator(tui);
            render_input(tui);
            continue;
        }

        if (b == 0x7f) {
            input_delete_before(tui);
            render_separator(tui);
            render_input(tui);
            continue;
        }

        if (b == '\033') {
            unsigned char b2;
            if (read_byte(&b2) != 0) continue;
            if (b2 != '[') {
                // Not a CSI sequence: recover the swallowed key so that e.g.
                // a lone ESC followed by a printable char does not lose it.
                if (b2 >= 0x20) {
                    input_insert(tui, (char)b2);
                    render_separator(tui);
                    render_input(tui);
                }
                continue;
            }
            unsigned char b3;
            if (read_byte(&b3) != 0) continue;

            if (b3 == 'D') {
                if (tui->input_cursor > 0) tui->input_cursor--;
                render_input(tui);
                continue;
            }
            if (b3 == 'C') {
                if (tui->input_cursor < tui->input_len) tui->input_cursor++;
                render_input(tui);
                continue;
            }
            if (b3 == 'A') {
                tui->scroll_offset++;
                int total = total_rendered_lines(tui);
                int history_rows = compute_history_rows(tui);
                int max_scroll = total - history_rows;
                if (max_scroll < 0) max_scroll = 0;
                if (tui->scroll_offset > max_scroll) tui->scroll_offset = max_scroll;
                render_history(tui);
                render_input(tui);
                continue;
            }
            if (b3 == 'B') {
                if (tui->scroll_offset > 0) tui->scroll_offset--;
                render_history(tui);
                render_input(tui);
                continue;
            }

            if (b3 >= '0' && b3 <= '9') {
                // CSI with parameters, e.g. ESC[3~ (Delete), ESC[5~ (PgUp),
                // ESC[6~ (PgDn) or modified keys like ESC[1;5C (Ctrl-Right).
                unsigned char b4;
                int params = 0;
                while (params++ < 16 && read_byte(&b4) == 0) {
                    if (b4 == '~') {
                        if (b3 == '3') {
                            input_delete_after(tui);
                            render_separator(tui);
                            render_input(tui);
                        } else if (b3 == '5') {
                            int history_rows = compute_history_rows(tui);
                            tui->scroll_offset += history_rows;
                            int total = total_rendered_lines(tui);
                            int max_scroll = total - history_rows;
                            if (max_scroll < 0) max_scroll = 0;
                            if (tui->scroll_offset > max_scroll) tui->scroll_offset = max_scroll;
                            render_history(tui);
                            render_input(tui);
                        } else if (b3 == '6') {
                            int history_rows = compute_history_rows(tui);
                            if (tui->scroll_offset >= history_rows) {
                                tui->scroll_offset -= history_rows;
                            } else {
                                tui->scroll_offset = 0;
                            }
                            render_history(tui);
                            render_input(tui);
                        }
                        break;
                    }
                    if (b4 == 'C') {
                        if (tui->input_cursor < tui->input_len) tui->input_cursor++;
                        render_input(tui);
                        break;
                    }
                    if (b4 == 'D') {
                        if (tui->input_cursor > 0) tui->input_cursor--;
                        render_input(tui);
                        break;
                    }
                    // Parameter bytes ('0'..'9', ';') and anything else are
                    // consumed until a final byte terminates the sequence.
                }
            }
            continue;
        }

        // Accept all printable bytes, including multi-byte UTF-8 sequences.
        // (Note: column counting is byte-based, so wrapping may split a
        // multi-byte character across lines.)
        if (b >= 0x20) {
            input_insert(tui, (char)b);
            render_separator(tui);
            render_input(tui);
            continue;
        }
    }
}
