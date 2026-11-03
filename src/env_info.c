/**
 * @file env_info.c
 * @brief Collects environment information (OS, cwd, local time) for the
 *        system message.
 *
 * POSIX-only symbols (uname, getcwd) are declared explicitly instead of
 * enabling feature-test macros (see AGENTS.md portability rules). The
 * <sys/utsname.h> header defines struct utsname unconditionally; only
 * the function prototype is hidden under -std=c99.
 */

#include "env_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

struct utsname;
extern int uname(struct utsname *buf);
extern char *getcwd(char *buf, size_t size);

/* Fixed overhead of the block: separator, headers, OS fields (uname
 * members are at most 65 bytes each on Linux) and the date line. */
#define ENV_INFO_FIXED_SLACK 512u

/**
 * @brief Returns the current working directory as a heap string.
 * @return Newly allocated string owned by the caller, or NULL if the
 *         directory cannot be determined (e.g. deleted) or on OOM.
 */
static char *env_info_getcwd(void)
{
	size_t cap;

	for (cap = 512; cap <= 8192; cap *= 2) {
		char *buf = malloc(cap);

		if (buf == NULL)
			return NULL;
		if (getcwd(buf, cap) != NULL)
			return buf;
		free(buf);
	}
	return NULL;
}

char *env_info_build(void)
{
	struct utsname uts;
	int have_uname;
	char *cwd = NULL;
	char *out = NULL;
	size_t need;
	size_t off = 0;
	int w;

	have_uname = (uname(&uts) == 0);
	cwd = env_info_getcwd();

	need = ENV_INFO_FIXED_SLACK + (cwd != NULL ? strlen(cwd) : 0);
	out = malloc(need);
	if (out == NULL)
		goto cleanup;

	w = snprintf(out, need, "\n\nEnvironment:\n");
	if (w < 0 || (size_t)w >= need) {
		/* The buffer is not NUL-terminated: drop it entirely. */
		free(out);
		out = NULL;
		goto cleanup;
	}
	off = (size_t)w;

	if (have_uname) {
		w = snprintf(out + off, need - off, "- OS: %s %s %s\n",
			     uts.sysname, uts.release, uts.machine);
		if (w < 0 || (size_t)w >= need - off)
			fprintf(stderr,
				"emma: warning: OS info truncated, omitted\n");
		else
			off += (size_t)w;
	}

	if (cwd != NULL)
		w = snprintf(out + off, need - off,
			     "- Working directory: %s\n", cwd);
	else
		w = snprintf(out + off, need - off,
			     "- Working directory: (unavailable)\n");
	if (w < 0 || (size_t)w >= need - off)
		fprintf(stderr,
			"emma: warning: cwd info truncated, omitted\n");
	else
		off += (size_t)w;

	{
		time_t now = time(NULL);
		struct tm *tm = localtime(&now);
		char dbuf[32];

		if (tm != NULL &&
		    strftime(dbuf, sizeof(dbuf), "%Y-%m-%d %H:%M", tm) > 0) {
			w = snprintf(out + off, need - off,
				     "- Date: %s (local time)\n", dbuf);
			if (w < 0 || (size_t)w >= need - off)
				fprintf(stderr,
					"emma: warning: date truncated, omitted\n");
		}
	}

cleanup:
	free(cwd);
	return out;
}
