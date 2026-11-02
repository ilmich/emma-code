/**
 * @file util.c
 * @brief Small shared helpers common to all modules.
 */

#include "util.h"

#include <stdlib.h>
#include <string.h>

char *util_strdup(const char *s)
{
	size_t n;
	char *copy;

	if (s == NULL)
		return NULL;
	n = strlen(s) + 1;
	copy = malloc(n);
	if (copy != NULL)
		memcpy(copy, s, n);
	return copy;
}
