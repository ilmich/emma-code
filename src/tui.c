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
#define ANSI_CLEAR_LINE    "\033[K"
#define ANSI_CLEAR_END     "\033[J"
#define ANSI_HIDE_CURSOR   "\033[?25l"
#define ANSI_SHOW_CURSOR   "\033[?25h"
#define INPUT_LABEL_LEN    5 /* "You: " */
#define USER_BLOCK_PAD     1 /* columns of padding inside message blocks */

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

    theme_t        theme;

    char          *status;

    char          *input_buf;
    size_t         input_len;
    size_t         input_cap;
    size_t         input_cursor;

    /* Transcript-mode state: finished messages are printed once into the
     * normal terminal scrollback; only the separator row and the input
     * box (the "live area") are repainted in place at the bottom.
     *
     * streaming_active: the last message is an assistant message still
     * being built from streamed deltas.
     * printed_lines: display lines of that message already emitted to
     * the transcript; the trailing (still growing) line stays in the
     * live area until the next delta or the commit.
     * cursor_row: rows below the live-area top the terminal cursor sits
     * on after the last paint; lets the next erase use relative movement
     * only (the absolute scroll position is unknown). */
    int            streaming_active;
    int            printed_lines;
    int            cursor_row;
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

// Cursor-relative movement helpers: the transcript model never knows the
// absolute scroll position, so the live area is erased and redrawn using
// only relative moves from the recorded cursor row.
static void cursor_up(int n) {
    char buf[16];
    if (n <= 0) return;
    int m = snprintf(buf, sizeof(buf), "\033[%dA", n);
    if (m > 0 && m < (int)sizeof(buf)) {
        out_write(buf, (size_t)m);
    }
}

static void cursor_right(int n) {
    char buf[16];
    if (n <= 0) return;
    int m = snprintf(buf, sizeof(buf), "\033[%dC", n);
    if (m > 0 && m < (int)sizeof(buf)) {
        out_write(buf, (size_t)m);
    }
}

static void write_str(const char *s) {
    out_write(s, strlen(s));
}

// Word-wraps plain text into unstyled lines owned by the caller (free
// with md_lines_free()). Returns NULL when there is nothing to render.
// Newlines force a line break; words longer than the width are
// hard-broken at the wrapping width.
static md_line_t *wrap_plain_lines(const char *text, int width,
                                   size_t *out_count) {
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

        row = malloc(seg_len + 1);
        if (!row) {
            md_lines_free(lines, count);
            return NULL;
        }
        memcpy(row, text + start, seg_len);
        row[seg_len] = '\0';
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

// Word-wraps plain text and styles every line with an SGR prefix and a
// reset suffix (free with md_lines_free()). A NULL style just adds the
// reset. Returns NULL when there is nothing to render or on allocation
// failure.
static md_line_t *wrap_styled_lines(const char *text, const char *style,
                                    int width, size_t *out_count) {
    const size_t reset_len = sizeof(ANSI_RESET) - 1;
    md_line_t *lines = wrap_plain_lines(text, width, out_count);
    size_t count;
    size_t style_len;

    if (lines == NULL) return NULL;
    count = (out_count != NULL) ? *out_count : 0;
    style_len = (style != NULL) ? strlen(style) : 0;

    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(lines[i].text);
        char *row = malloc(style_len + len + reset_len + 1);
        if (!row) {
            md_lines_free(lines, count);
            if (out_count) *out_count = 0;
            return NULL;
        }
        memcpy(row, style, style_len);
        memcpy(row + style_len, lines[i].text, len);
        memcpy(row + style_len + len, ANSI_RESET, reset_len + 1);
        free(lines[i].text);
        lines[i].text = row;
    }
    return lines;
}

static int count_input_lines(tui_t *tui) {
    int first_width = tui->term_cols - INPUT_LABEL_LEN;
    int line = 0, col = 0;

    if (first_width < 1) first_width = 1;
    if (tui->input_len == 0) return 1;

    tui_input_line_col(tui->input_buf, tui->input_len, tui->input_len,
                       first_width, tui->term_cols, &line, &col);
    return line + 1;
}

static int compute_input_box_height(tui_t *tui) {
    int lines = count_input_lines(tui);
    if (lines < 1) lines = 1;
    if (lines > INPUT_MAX_LINES) lines = INPUT_MAX_LINES;
    return lines;
}

// Columns of horizontal padding inside a message block: user messages
// carry it inside their background block, every other role as a plain
// left indent. The padding shrinks to zero on terminals too narrow to
// hold it.
static int block_pad(int term_cols) {
    return (term_cols > 2 * USER_BLOCK_PAD) ? USER_BLOCK_PAD : 0;
}

// Wrapping width for message content. User messages render inside a
// full-width background block with USER_BLOCK_PAD columns of padding per
// side; every other role is indented by the same USER_BLOCK_PAD columns
// (messages carry no role labels anymore).
static int message_content_width(const tui_t *tui, const char *role) {
    int width = tui->term_cols;
    (void)role;
    width -= 2 * block_pad(tui->term_cols);
    return (width >= 1) ? width : 1;
}

// Frees an array built by build_message_lines(). NULL-safe; tolerates
// NULL entries.
static void free_message_lines(char **lines, size_t count) {
    if (!lines) return;
    for (size_t i = 0; i < count; i++) {
        free(lines[i]);
    }
    free(lines);
}

// Skips the leading CR/LF characters of a string. Chat APIs often
// start the assistant reply (content and reasoning alike) with blank
// lines; rendering them would show a spurious empty row at the top of
// every assistant section. Returns the pointer past the leading breaks
// (NULL-safe; an all-breaks string yields the empty string).
static const char *skip_leading_breaks(const char *s) {
    if (s == NULL) return NULL;
    while (*s == '\n' || *s == '\r') s++;
    return s;
}

// Builds the display lines of one message. User messages render as a
// full-width background block: an empty background row, the wrapped
// plain text between USER_BLOCK_PAD columns of background padding per
// side, and another empty background row (so the block stays visible
// while scrolling the history). Assistant and system messages render as
// plain text indented by USER_BLOCK_PAD columns (no background, no edge
// rows): the styled reasoning block (italic, no label row), one empty
// indented line, then the markdown-rendered content.
// Empty or NULL content yields just the block edges (user) or one empty
// row -- except when streaming is nonzero: then the trailing empty row
// is skipped entirely, so the last display line is the still-growing
// tail of the streamed text (a phantom row would make the line before
// it look stable and get emitted while it can still re-wrap, corrupting
// the transcript).
//
// Returns a malloc'd array of malloc'd styled lines (free with
// free_message_lines()), or NULL when there is nothing to render or on
// allocation failure (*out_count is 0 in both cases).
static char **build_message_lines(const tui_t *tui, const char *role,
                                  const char *content, const char *reasoning,
                                  int streaming, size_t *out_count) {
    md_line_t *body_lines = NULL;
    md_line_t *md_lines = NULL;
    size_t body_count = 0;
    size_t md_count = 0;
    size_t total = 0;
    char **lines = NULL;
    int is_user;
    int width;
    int pad;
    int sep;

    if (out_count) *out_count = 0;

    is_user = (role != NULL && strcmp(role, "user") == 0);
    width = message_content_width(tui, role);
    pad = block_pad(tui->term_cols);

    if (is_user) {
        /* User block: plain wrapped text between two empty background
         * rows. Reasoning is never rendered for user messages. */
        body_lines = wrap_plain_lines(content, width, &body_count);
        total = body_count + 2;
    } else {
        /* Trim the leading blank lines the API prepends to assistant
         * replies so sections do not start with an empty row. */
        reasoning = skip_leading_breaks(reasoning);
        content = skip_leading_breaks(content);
        body_lines = wrap_styled_lines(reasoning, tui->theme.reasoning_on,
                                       width, &body_count);
        md_lines = md_render(content, width, &md_count);
        /* One empty indented line separates the reasoning block from
         * the content (the reasoning's own trailing newline is dropped
         * by the wrapper, so it must be added back here). */
        sep = (body_count > 0 && md_count > 0) ? 1 : 0;
        total = body_count + (size_t)sep +
                ((md_count > 0) ? md_count : (streaming ? 0 : 1));
    }
    if (total == 0) {
        md_lines_free(body_lines, body_count);
        md_lines_free(md_lines, md_count);
        return NULL;
    }

    lines = malloc(total * sizeof(char *));
    if (!lines) {
        md_lines_free(body_lines, body_count);
        md_lines_free(md_lines, md_count);
        return NULL;
    }
    memset(lines, 0, total * sizeof(char *));

    for (size_t i = 0; i < total; i++) {
        const char *body;
        size_t body_len;
        size_t bg_len = strlen(tui->theme.user_bg);
        size_t clear_len = sizeof(ANSI_CLEAR_LINE) - 1;
        size_t reset_len = sizeof(ANSI_RESET) - 1;
        char *row;

        if (is_user) {
            if (i == 0 || i == total - 1) {
                /* Empty full-width background row (block edge). */
                body = "";
                body_len = 0;
            } else {
                body = (body_lines[i - 1].text != NULL)
                           ? body_lines[i - 1].text : "";
                body_len = strlen(body);
            }
            /* Background + left padding + text + right padding, then
             * ANSI_CLEAR_LINE fills the rest of the row with the block
             * background. */
            row = malloc(bg_len + 2 * (size_t)pad + body_len +
                         clear_len + reset_len + 1);
            if (!row) {
                free_message_lines(lines, total);
                md_lines_free(body_lines, body_count);
                md_lines_free(md_lines, md_count);
                return NULL;
            }
            memcpy(row, tui->theme.user_bg, bg_len);
            for (int p = 0; p < pad; p++) row[bg_len + (size_t)p] = ' ';
            memcpy(row + bg_len + (size_t)pad, body, body_len);
            for (int p = 0; p < pad; p++) {
                row[bg_len + (size_t)pad + body_len + (size_t)p] = ' ';
            }
            memcpy(row + bg_len + 2 * (size_t)pad + body_len,
                   ANSI_CLEAR_LINE, clear_len);
            memcpy(row + bg_len + 2 * (size_t)pad + body_len + clear_len,
                   ANSI_RESET, reset_len + 1);
            lines[i] = row;
        } else {
            int is_sep = (sep && i == body_count);

            if (is_sep) {
                body = "";
            } else if (i < body_count) {
                body = (body_lines[i].text != NULL)
                           ? body_lines[i].text : "";
            } else {
                size_t j = i - body_count - (size_t)sep;
                body = (j < md_count && md_lines[j].text != NULL)
                           ? md_lines[j].text : "";
            }
            body_len = strlen(body);

            /* Plain left indent instead of the user block's background
             * padding. Empty lines stay truly empty (no trailing
             * whitespace) -- except the reasoning/content separator,
             * which carries the indent like every other line of the
             * block. */
            if (body_len == 0 && !is_sep) {
                lines[i] = malloc(1);
                if (!lines[i]) {
                    free_message_lines(lines, total);
                    md_lines_free(body_lines, body_count);
                    md_lines_free(md_lines, md_count);
                    return NULL;
                }
                lines[i][0] = '\0';
                continue;
            }
            lines[i] = malloc((size_t)pad + body_len + 1);
            if (!lines[i]) {
                free_message_lines(lines, total);
                md_lines_free(body_lines, body_count);
                md_lines_free(md_lines, md_count);
                return NULL;
            }
            for (int p = 0; p < pad; p++) lines[i][p] = ' ';
            memcpy(lines[i] + pad, body, body_len + 1);
        }
    }

    md_lines_free(body_lines, body_count);
    md_lines_free(md_lines, md_count);
    if (out_count) *out_count = total;
    return lines;
}

// Erases the live area (any in-progress streaming line, the separator
// row and the input box) using movement relative to the recorded cursor
// row, leaving the cursor at its top on a cleared screen position.
static void erase_live_area(tui_t *tui) {
    write_str("\r");
    cursor_up(tui->cursor_row);
    write_str(ANSI_CLEAR_END);
    tui->cursor_row = 0;
}

// Draws the separator row: the status text when set, otherwise a full
// row of dashes. Moves to the next line afterwards.
static void draw_separator(tui_t *tui) {
    if (tui->status && tui->status[0]) {
        write_str(tui->theme.status);
        write_str(" ");
        write_str(tui->status);
        write_str(ANSI_RESET);
        write_str(ANSI_CLEAR_LINE);
    } else {
        for (int i = 0; i < tui->term_cols; i++) {
            char ch[2] = {SEPARATOR_CHAR, 0};
            write_str(ch);
        }
    }
    write_str("\r\n");
}

// Draws the visible window of the multi-line input box at the current
// cursor position (column 1). Outputs the 0-based visible line and column
// of the input cursor (the column includes the label offset on the first
// visible row) so the caller can place the terminal cursor afterwards.
static void draw_input_box(tui_t *tui, int input_h, int *out_vl,
                           int *out_col0) {
    int first_width = tui->term_cols - INPUT_LABEL_LEN;
    int cursor_line = 0, cursor_col = 0;
    int start_line;
    int line_idx = 0, col = 0, first_line = 1;
    int effective_width;
    int prev_rel = -1;
    int vl = 0, col0 = 0;
    const char *buf = tui->input_buf;

    if (first_width < 1) first_width = 1;
    effective_width = first_width;

    // Logical line the cursor sits on, so the visible window can be chosen
    // to keep it inside the box (same wrapping rules as the draw loop and
    // count_input_lines() below).
    tui_input_line_col(tui->input_buf, tui->input_len, tui->input_cursor,
                       first_width, tui->term_cols, &cursor_line,
                       &cursor_col);
    start_line = cursor_line - (input_h - 1);
    if (start_line < 0) start_line = 0;

    if (start_line == 0) {
        write_str(tui->theme.input);
        write_str("You: ");
        write_str(ANSI_RESET);
    }

    for (size_t i = 0; i <= tui->input_len; i++) {
        int rel = line_idx - start_line;
        if (i == tui->input_cursor) {
            vl = (rel > 0) ? rel : 0;
            col0 = col + ((line_idx == 0 && start_line == 0)
                              ? INPUT_LABEL_LEN : 0);
        }
        if (i == tui->input_len) break;

        char c = buf[i];

        if (c == '\n') {
            line_idx++;
            col = 0;
            first_line = 0;
            effective_width = tui->term_cols;
            if (line_idx - start_line >= input_h) break;
            continue;
        }

        if (rel >= 0 && rel < input_h) {
            if (rel != prev_rel) {
                if (prev_rel >= 0) write_str("\r\n");
                prev_rel = rel;
            }
            out_write(buf + i, 1);
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

    // Make sure the cursor's row exists on screen even when it holds no
    // characters (e.g. an empty last line after a trailing newline).
    while (prev_rel < vl) {
        if (prev_rel >= 0) write_str("\r\n");
        prev_rel++;
    }

    *out_vl = vl;
    *out_col0 = col0;
}

// Redraws the live area below the transcript: the trailing partial line
// of an in-progress streamed message (when present), the separator row
// and the input box. Leaves the terminal cursor at the input position and
// records its live-area-relative row for the next erase.
static void draw_live_area(tui_t *tui, const char *tail) {
    int input_h = compute_input_box_height(tui);
    int overhead = (tail != NULL ? 1 : 0) + 1; /* tail row + separator */
    int vl = 0, col0 = 0;

    /* Keep the live area inside the screen on very small terminals. */
    if (input_h > tui->term_rows - overhead) {
        input_h = tui->term_rows - overhead;
    }
    if (input_h < 1) input_h = 1;

    write_str(ANSI_HIDE_CURSOR);

    if (tail != NULL) {
        write_str(tail);
        write_str(ANSI_CLEAR_LINE);
        write_str("\r\n");
    }

    draw_separator(tui);

    draw_input_box(tui, input_h, &vl, &col0);
    write_str(ANSI_CLEAR_LINE);

    cursor_up((input_h - 1) - vl);
    write_str("\r");
    cursor_right(col0);
    tui->cursor_row = (tail != NULL ? 1 : 0) + 1 + vl;

    write_str(ANSI_SHOW_CURSOR);
}

// Current trailing unemitted line of the in-progress message, as a
// malloc'd string (NULL when nothing is held back or on allocation
// failure).
static char *current_tail(tui_t *tui) {
    char **lines = NULL;
    size_t count = 0;
    char *tail = NULL;

    if (tui->msg_count == 0) return NULL;
    lines = build_message_lines(tui,
                                tui->messages[tui->msg_count - 1].role,
                                tui->messages[tui->msg_count - 1].content,
                                tui->messages[tui->msg_count - 1].reasoning,
                                1, &count);
    if (lines != NULL && count > 0 &&
        (size_t)tui->printed_lines < count) {
        tail = strdup(lines[count - 1]);
    }
    free_message_lines(lines, count);
    return tail;
}

// Erases and redraws the live area, keeping the trailing partial line of
// an in-progress streamed message visible above the separator.
static void refresh_live_area(tui_t *tui) {
    char *tail = tui->streaming_active ? current_tail(tui) : NULL;

    erase_live_area(tui);
    draw_live_area(tui, tail);
    free(tail);
}

// Brings the terminal up to date with the in-progress streamed message:
// re-renders it and emits every newly completed display line to the
// transcript (all remaining lines, including the trailing partial one,
// when flush_all is set -- otherwise all but the last, which stays in
// the live area as the visible "typing" line). Always repaints the live
// area afterwards. A flushed stream is finished: the streaming state is
// cleared and a blank line is left after the message.
static void stream_sync(tui_t *tui, int flush_all) {
    char **lines = NULL;
    size_t count = 0;
    char *tail = NULL;

    if (tui->msg_count > 0 && tui->messages[tui->msg_count - 1].role != NULL &&
        strcmp(tui->messages[tui->msg_count - 1].role, "assistant") == 0) {
        /* Streaming build (no phantom trailing row) while the message
         * is still growing; the commit flush uses the full render. */
        lines = build_message_lines(tui, "assistant",
                                    tui->messages[tui->msg_count - 1].content,
                                    tui->messages[tui->msg_count - 1].reasoning,
                                    !flush_all, &count);
    }

    erase_live_area(tui);

    if (lines != NULL && count > 0) {
        int emit_to = tui_stream_emit_to((int)count, flush_all);
        for (int i = tui->printed_lines; i <= emit_to && (size_t)i < count;
             i++) {
            write_str(lines[i]);
            write_str("\r\n");
        }
        if (emit_to >= tui->printed_lines) {
            tui->printed_lines = emit_to + 1;
            if (tui->printed_lines > (int)count) {
                tui->printed_lines = (int)count;
            }
        }
        /* The trailing line stays in the live area: copy it before the
         * array is freed below. */
        if (!flush_all && (size_t)tui->printed_lines < count) {
            tail = strdup(lines[count - 1]);
        }
    }
    free_message_lines(lines, count);

    if (flush_all) {
        write_str("\r\n"); /* blank line after the finished message */
        tui->streaming_active = 0;
        tui->printed_lines = 0;
        draw_live_area(tui, NULL);
    } else {
        draw_live_area(tui, tail);
        free(tail);
    }
}

// Prints one committed message into the transcript (scrollback): flushes
// any in-progress streamed message first, erases the live area, writes
// the message's display lines followed by one blank line and repaints
// the live area.
static void print_message(tui_t *tui, const char *role, const char *content,
                          const char *reasoning) {
    if (tui->streaming_active) stream_sync(tui, 1);

    erase_live_area(tui);

    size_t count = 0;
    char **lines = build_message_lines(tui, role, content, reasoning,
                                       0, &count);
    if (lines != NULL) {
        for (size_t i = 0; i < count; i++) {
            write_str(lines[i]);
            write_str("\r\n");
        }
        free_message_lines(lines, count);
    }
    write_str("\r\n"); /* blank line between transcript messages */
    draw_live_area(tui, NULL);
}

void tui_render(tui_t *tui) {
    if (!tui) return;
    refresh_live_area(tui);
    out_flush();
}

int tui_init(tui_t **out, const theme_t *theme) {
    tui_t *tui = calloc(1, sizeof(tui_t));
    int rc = -1;

    if (!tui) {
        fprintf(stderr, "tui_init: calloc failed\n");
        return -1; /* nothing to clean up yet */
    }

    if (theme_default(&tui->theme) != 0) {
        fprintf(stderr, "tui_init: theme_default failed\n");
        goto cleanup;
    }
    if (theme != NULL && theme_copy(&tui->theme, theme) != 0) {
        fprintf(stderr, "tui_init: theme_copy failed\n");
        goto cleanup;
    }

    tui->messages = malloc(MESSAGES_INIT * sizeof(tui_message_t));
    if (!tui->messages) {
        fprintf(stderr, "tui_init: malloc messages failed\n");
        goto cleanup;
    }
    tui->msg_cap = MESSAGES_INIT;

    tui->input_cap = INPUT_BUF_INIT;
    /* calloc: the draw loop reads buffer bytes for rendering; zeroed
     * memory keeps every byte initialized from the start. */
    tui->input_buf = calloc(1, tui->input_cap);
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

    /* Transcript mode: never clear the screen. Start on a fresh line
     * below the shell prompt; the conversation flows into the normal
     * terminal scrollback from here. */
    write_str("\r\n");

    *out = tui;
    tui = NULL; /* ownership transferred to the caller */
    rc = 0;

cleanup:
    if (tui != NULL) {
        theme_free(&tui->theme);
        free(tui->input_buf);
        free(tui->messages);
        free(tui);
    }
    return rc;
}

void tui_free(tui_t *tui) {
    if (!tui) return;

    tui_exit_raw(tui);
    /* Leave the transcript visible: move below the input box, restore
     * the cursor and let the shell prompt print on a fresh line. */
    write_str("\r\n");
    write_str(ANSI_SHOW_CURSOR);
    out_flush();

    for (size_t i = 0; i < tui->msg_count; i++) {
        free(tui->messages[i].role);
        free(tui->messages[i].content);
        free(tui->messages[i].reasoning);
    }
    theme_free(&tui->theme);
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

// Appends a message to the in-memory history without printing it.
// Returns 0 on success, -1 on allocation failure (nothing appended).
static int append_message_store(tui_t *tui, const char *role,
                                const char *content) {
    if (tui->msg_count >= tui->msg_cap) {
        size_t new_cap = tui->msg_cap * 2;
        tui_message_t *tmp = realloc(tui->messages, new_cap * sizeof(tui_message_t));
        if (!tmp) {
            fprintf(stderr, "tui_append_message: realloc failed\n");
            return -1;
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
        return -1;
    }

    tui->messages[tui->msg_count].role    = r;
    tui->messages[tui->msg_count].content = c;
    tui->messages[tui->msg_count].reasoning = NULL;
    tui->msg_count++;
    return 0;
}

void tui_append_message(tui_t *tui, const char *role, const char *content) {
    if (!tui || !role || !content) return;

    if (append_message_store(tui, role, content) != 0) return;

    print_message(tui, role, content, NULL);
    out_flush();
}

void tui_stream_reasoning_append(tui_t *tui, const char *text) {
    if (!tui || !text || !*text) return;

    if (!(tui->streaming_active && tui->msg_count > 0 &&
          tui->messages[tui->msg_count - 1].role != NULL &&
          strcmp(tui->messages[tui->msg_count - 1].role, "assistant") == 0)) {
        // No assistant message in progress: the first reasoning delta
        // starts one (content stays empty until the first content delta
        // arrives). It is not printed yet: it exists only as the
        // streaming target until its display lines complete.
        if (append_message_store(tui, "assistant", "") != 0) return;
        tui->streaming_active = 1;
        tui->printed_lines = 0;
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

    stream_sync(tui, 0);
    out_flush();
}

void tui_stream_append(tui_t *tui, const char *text) {
    if (!tui || !text || !*text) return;

    if (tui->streaming_active && tui->msg_count > 0 &&
        tui->messages[tui->msg_count - 1].role != NULL &&
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
        // No assistant message in progress: the first delta starts one
        // (kept out of the transcript until its lines complete).
        if (append_message_store(tui, "assistant", text) != 0) return;
        tui->streaming_active = 1;
        tui->printed_lines = 0;
    }

    stream_sync(tui, 0);
    out_flush();
}

void tui_replace_assistant_message(tui_t *tui, const char *content,
                                   const char *reasoning) {
    if (!tui || !content) return;

    if (tui->streaming_active && tui->msg_count > 0 &&
        tui->messages[tui->msg_count - 1].role != NULL &&
        strcmp(tui->messages[tui->msg_count - 1].role, "assistant") == 0) {
        // Commit the final reply into the message the streamed deltas
        // have already created, so the text is not shown twice, and
        // flush every remaining line of it into the transcript.
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
        stream_sync(tui, 1);
        out_flush();
        return;
    }

    // No streamed assistant message in progress: append and print in one
    // go (the reasoning is display-only).
    if (append_message_store(tui, "assistant", content) != 0) return;
    if (tui->msg_count > 0 && reasoning != NULL && reasoning[0] != '\0') {
        tui_message_t *m = &tui->messages[tui->msg_count - 1];
        m->reasoning = strdup(reasoning);
        if (m->reasoning == NULL) {
            fprintf(stderr, "tui_replace_assistant_message: strdup failed\n");
        }
    }
    print_message(tui, "assistant", content, reasoning);
    out_flush();
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
    refresh_live_area(tui);
    out_flush();
}

int tui_confirm(tui_t *tui, const char *prompt) {
    if (!tui || !prompt) return 0;

    // Print the prompt as a transcript line below the live area so the
    // question and its answer stay in the scrollback.
    erase_live_area(tui);
    char line[256];
    int n = snprintf(line, sizeof(line), "%s [y/n] ", prompt);
    if (n < 0 || (size_t)n >= sizeof(line)) {
        write_str("[confirm prompt truncated] [y/n] ");
    } else {
        write_str(line);
    }
    out_flush();

    int answer = 0;
    while (1) {
        unsigned char b;
        if (read_byte(&b) != 0) {
            answer = 0; /* EOF/terminal gone: deny */
            break;
        }
        if (b == 'y' || b == 'Y') {
            answer = 1;
            break;
        }
        if (b == 'n' || b == 'N') {
            answer = 0;
            break;
        }
        if (b == 0x03) {
            /* Ctrl-C: deny like every other escape path. */
            answer = 0;
            break;
        }
        /* Any other key: keep waiting. */
    }
    write_str(answer ? "y\r\n" : "n\r\n");
    refresh_live_area(tui);
    out_flush();
    return answer;
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

void tui_input_line_col(const char *buf, size_t len, size_t index,
                        int first_width, int width, int *out_line,
                        int *out_col) {
    int line = 0, col = 0, first_line = 1;
    int effective;

    if (first_width < 1) first_width = 1;
    if (width < 1) width = 1;
    effective = first_width;
    if (index > len) index = len;

    for (size_t i = 0; i < index; i++) {
        char c = buf[i];
        if (c == '\n') {
            line++;
            col = 0;
            first_line = 0;
            effective = width;
        } else {
            col++;
            if (col >= effective) {
                col = 0;
                line++;
                if (first_line) {
                    first_line = 0;
                    effective = width;
                }
            }
        }
    }
    if (out_line) *out_line = line;
    if (out_col) *out_col = col;
}

int tui_input_index_at(const char *buf, size_t len, int first_width,
                       int width, int line, int col, size_t *out_index) {
    int cur_line = 0, cur_col = 0, first_line = 1;
    int effective;
    size_t i = 0;

    if (!buf) return -1;
    if (first_width < 1) first_width = 1;
    if (width < 1) width = 1;
    effective = first_width;
    if (line < 0) return -1;

    /* Walk to the start of the requested visual line. Line breaks follow
     * the input box wrapping rules: a newline or reaching the wrapping
     * width starts a new line. */
    while (i < len && cur_line < line) {
        char c = buf[i];
        if (c == '\n') {
            cur_line++;
            cur_col = 0;
            first_line = 0;
            effective = width;
        } else {
            cur_col++;
            if (cur_col >= effective) {
                cur_col = 0;
                cur_line++;
                if (first_line) {
                    first_line = 0;
                    effective = width;
                }
            }
        }
        i++;
    }
    if (cur_line < line) return -1; /* line beyond the input content */

    /* Walk within the line up to the requested column, clamped to the
     * line's end (a newline or a wrap boundary). */
    int walked = 0;
    while (i < len && walked < col && walked < effective) {
        if (buf[i] == '\n') break;
        i++;
        walked++;
    }
    if (out_index) *out_index = i;
    return 0;
}

int tui_stream_emit_to(int total_lines, int flush_all) {
    if (total_lines <= 0) return -1;
    if (flush_all) return total_lines - 1;
    if (total_lines < 2) return -1; /* only the trailing line exists */
    return total_lines - 2;
}

// Moves the input cursor up (delta < 0) or down (delta > 0) by one
// visual line, keeping the logical column clamped to the target line's
// end. No-op at the first/last line.
static void input_move_vertical(tui_t *tui, int delta) {
    int first_width = tui->term_cols - INPUT_LABEL_LEN;
    int line = 0, col = 0;
    size_t idx = 0;

    if (first_width < 1) first_width = 1;
    if (delta == 0) return;

    tui_input_line_col(tui->input_buf, tui->input_len, tui->input_cursor,
                       first_width, tui->term_cols, &line, &col);
    int target = line + delta;
    if (target < 0) return;
    if (tui_input_index_at(tui->input_buf, tui->input_len, first_width,
                           tui->term_cols, target, col, &idx) != 0) {
        return; /* already on the last line */
    }
    tui->input_cursor = idx;
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
            refresh_live_area(tui);
            continue;
        }

        if (b == 0x7f) {
            input_delete_before(tui);
            refresh_live_area(tui);
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
                    refresh_live_area(tui);
                }
                continue;
            }
            unsigned char b3;
            if (read_byte(&b3) != 0) continue;

            if (b3 == 'D') {
                if (tui->input_cursor > 0) tui->input_cursor--;
                refresh_live_area(tui);
                continue;
            }
            if (b3 == 'C') {
                if (tui->input_cursor < tui->input_len) tui->input_cursor++;
                refresh_live_area(tui);
                continue;
            }
            if (b3 == 'A') {
                // Up: move the cursor one visual line up inside the
                // multi-line input box (scrolling is native now).
                input_move_vertical(tui, -1);
                refresh_live_area(tui);
                continue;
            }
            if (b3 == 'B') {
                input_move_vertical(tui, 1);
                refresh_live_area(tui);
                continue;
            }

            if (b3 >= '0' && b3 <= '9') {
                // CSI with parameters, e.g. ESC[3~ (Delete) or modified
                // keys like ESC[1;5C (Ctrl-Right). PgUp/PgDn (ESC[5~ /
                // ESC[6~) are ignored: the terminal scrolls natively.
                unsigned char b4;
                int params = 0;
                while (params++ < 16 && read_byte(&b4) == 0) {
                    if (b4 == '~') {
                        if (b3 == '3') {
                            input_delete_after(tui);
                            refresh_live_area(tui);
                        }
                        break;
                    }
                    if (b4 == 'C') {
                        if (tui->input_cursor < tui->input_len) tui->input_cursor++;
                        refresh_live_area(tui);
                        break;
                    }
                    if (b4 == 'D') {
                        if (tui->input_cursor > 0) tui->input_cursor--;
                        refresh_live_area(tui);
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
            refresh_live_area(tui);
            continue;
        }
    }
}
