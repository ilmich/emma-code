#ifndef MARKDOWN_H
#define MARKDOWN_H

#include <stddef.h>

/**
 * @file markdown.h
 * @brief Minimal markdown parser/renderer for the terminal UI.
 *
 * Converts markdown text into an array of pre-wrapped, fully styled
 * terminal lines (SGR escape sequences embedded, no trailing newline).
 * Supported constructs: headings (#..######), bold/italic, inline code,
 * fenced code blocks, bullet/ordered lists, blockquotes, links
 * ("text (url)") and horizontal rules. Unclosed constructs degrade
 * gracefully (rendered literally), which keeps streaming output stable.
 */

/** One rendered, pre-wrapped output line (styled text, owned). */
typedef struct {
	char *text;
} md_line_t;

/**
 * @brief Renders markdown text into styled, pre-wrapped terminal lines.
 *
 * @param content   Markdown source (may be NULL or empty).
 * @param width     Wrapping column count (clamped to at least 1).
 * @param out_count Receives the number of returned lines (may be NULL).
 * @return Array of md_line_t owned by the caller (free with
 *         md_lines_free()), or NULL when content is NULL/empty or
 *         nothing could be rendered.
 */
md_line_t *md_render(const char *content, int width, size_t *out_count);

/**
 * @brief Frees an array returned by md_render().
 *
 * NULL-safe: passing NULL (or count 0) is a no-op.
 */
void md_lines_free(md_line_t *lines, size_t count);

#endif /* MARKDOWN_H */
