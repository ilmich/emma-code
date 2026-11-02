#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

/**
 * @brief Duplicates a NUL-terminated string into a fresh allocation.
 *
 * @param s String to copy (must not be NULL).
 * @return Newly allocated copy owned by the caller (free with free()),
 *         or NULL when s is NULL or on allocation failure.
 */
char *util_strdup(const char *s);

#endif /* UTIL_H */
