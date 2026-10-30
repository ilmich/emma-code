/**
 * @file toml.h
 * @brief Minimal TOML parser: generic table/value API for config files.
 *
 * Supports the practical TOML subset: [table] and [table.sub] headers,
 * dotted keys, basic strings with escapes, literal strings, integers,
 * floats, booleans, single-line arrays of scalars, comments and blank
 * lines. Anything else (multiline strings, inline tables, datetimes,
 * [[array of tables]]) is rejected with a clear error message.
 */

#ifndef TOML_H
#define TOML_H

#include <stddef.h>

/** Discriminator for the value stored in a table entry. */
typedef enum {
	TOML_STRING, /**< NUL-terminated UTF-8 string. */
	TOML_INT,    /**< 64-bit signed integer. */
	TOML_DOUBLE, /**< IEEE-754 double. */
	TOML_BOOL,   /**< 0 or 1 stored in an int. */
	TOML_ARRAY,  /**< Heterogeneous value array (see toml_array_t). */
	TOML_TABLE   /**< Nested table (see toml_table_t). */
} toml_type_t;

/** Opaque parsed TOML document (the root table). */
typedef struct toml_table toml_table_t;

/** Untyped array value, as returned by toml_get() for TOML_ARRAY. */
typedef struct {
	const void *items; /**< Contiguous internal value items (borrowed). */
	size_t count;      /**< Number of items. */
} toml_array_t;

/**
 * Parse a NUL-terminated TOML document held in memory.
 *
 * @param src    Document text (must remain valid only for the call).
 * @param out    Receives the parsed root table on success.
 * @param errbuf Caller buffer for the error message (may be NULL).
 * @param errlen Size of errbuf.
 * @return 0 on success, -1 on parse error (errbuf receives the message).
 */
int toml_parse_string(const char *src, toml_table_t **out, char *errbuf,
		      size_t errlen);

/**
 * Parse a TOML file: reads it fully into memory, delegates to
 * toml_parse_string, then frees the buffer regardless of outcome.
 *
 * @param path   File path.
 * @param out    Receives the parsed root table on success.
 * @param errbuf Caller buffer for the error message (may be NULL).
 * @param errlen Size of errbuf.
 * @return 0 on success, -1 on open/read/parse error (errbuf receives
 *         the message).
 */
int toml_parse_file(const char *path, toml_table_t **out, char *errbuf,
		    size_t errlen);

/**
 * Look up a key by dotted path ("a.b.c") in a table.
 *
 * @param t    Parsed table.
 * @param key  Dotted key path.
 * @param type If non-NULL, receives the value type.
 * @return Const pointer to the payload: TOML_STRING yields the char
 *         buffer itself, TOML_INT/TOML_DOUBLE/TOML_BOOL yield pointers
 *         to the scalar, TOML_ARRAY yields a toml_array_t and
 *         TOML_TABLE yields a toml_table_t. NULL when the key is
 *         missing. The pointer borrows from t; never free it.
 */
const void *toml_get(const toml_table_t *t, const char *key,
		     toml_type_t *type);

/** Typed getter: 0 on success, -1 on missing key or type mismatch. */
int toml_get_string(const toml_table_t *t, const char *key,
		    const char **out);
/** Typed getter: 0 on success, -1 on missing key or type mismatch. */
int toml_get_int(const toml_table_t *t, const char *key, long long *out);
/** Typed getter: 0 on success, -1 on missing key or type mismatch. */
int toml_get_double(const toml_table_t *t, const char *key, double *out);
/** Typed getter: 0 on success, -1 on missing key or type mismatch. */
int toml_get_bool(const toml_table_t *t, const char *key, int *out);

/** Number of items in a toml_array_t. */
size_t toml_array_count(const toml_array_t *arr);

/**
 * Item accessor for a toml_array_t: same borrow semantics as toml_get().
 * Returns NULL when idx is out of range.
 */
const void *toml_array_item(const toml_array_t *arr, size_t idx,
			    toml_type_t *type);

/** Free a table returned by the parse functions (frees everything). */
void toml_free(toml_table_t *t);

#endif /* TOML_H */
