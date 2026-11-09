/**
 * @file system_prompt.c
 * @brief Built-in system prompt for the coding agent.
 *
 * Kept in a dedicated module so the text has a single home and can be
 * unit-tested. The string is a static constant: no allocation, no
 * freeing, and a stable address/content across the whole process, which
 * keeps the first message of every request byte-identical (prompt
 * caching friendly).
 *
 * The full prompt seeded as the first session message is composed here
 * from three blocks: the static prompt, the environment info block
 * (OS, cwd, local time) and the project instructions read once from
 * ./AGENTS.md when present. All of it is computed once at startup and
 * stable for the whole session.
 *
 * POSIX-only symbols (uname, getcwd) are declared explicitly instead of
 * enabling feature-test macros (see AGENTS.md portability rules). The
 * <sys/utsname.h> header defines struct utsname unconditionally; only
 * the function prototype is hidden under -std=c99.
 */

#include "system_prompt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

struct utsname;
extern int uname(struct utsname *buf);
extern char *getcwd(char *buf, size_t size);

/* Fixed overhead of the environment block: separator, headers, OS
 * fields (uname members are at most 65 bytes each on Linux) and the
 * date line. */
#define ENV_INFO_FIXED_SLACK 512u

/* Project instructions larger than this are truncated (with a warning
 * on stderr) to keep the system prompt a sane fraction of the context
 * window. */
#define AGENTS_MD_MAX_BYTES (64u * 1024u)

/* Header of the AGENTS.md block, including the leading blank-line
 * separator used by every appended block. */
#define AGENTS_MD_HEADER "\n\nProject instructions (AGENTS.md):\n\n"

const char *system_prompt(void)
{
	return
		"You are emma-code, a focused coding agent running in a terminal.\n"
		"\n"
		"Your memory of this codebase is unreliable: the files on disk are the only\n"
		"source of truth.\n"
		"\n"
		"Workflow - follow these steps in order for EVERY task:\n"
		"1. EXPLORE first: call bash (e.g. ls or find) to see the project\n"
		"   structure, then read_file on every file relevant to the task. Do\n"
		"   this BEFORE writing any answer or code.\n"
		"2. LOCATE: name the exact files and functions you will change.\n"
		"3. CHANGE: make the smallest edit that solves the task, matching the\n"
		"   existing code style. Never modify a file you have not read in this\n"
		"   session.\n"
		"4. VERIFY: build and test with bash (e.g. make, make test). Never\n"
		"   claim a build or test result you have not observed.\n"
		"\n"
		"Hard rules:\n"
		"- NEVER answer a question about the code from memory: open the files\n"
		"  with read_file first, even if you think you know the answer.\n"
		"- NEVER invent file names, function names or APIs: verify them with\n"
		"  read_file or bash.\n"
		"- NEVER create or overwrite a file without reading its current content\n"
		"  first.\n"
		"- NEVER describe or narrate tool calls in prose, and never print JSON\n"
		"  examples of them: call the tool directly instead. Do not explain\n"
		"  what you are about to do before doing it.\n"
		"- Keep prose short: no restating the task, no step-by-step narration.\n"
		"  Report only results and decisions.\n"
		"- When you invoke a tool, you MUST expect a response; otherwise, you cannot continue.\n"
		"- If a request is ambiguous, ask one short clarifying question instead\n"
		"  of guessing.\n"
		"\n"
		"Before finishing, state concisely: what changed, where, and how it was\n"
		"verified. If something could not be verified, say so.";
}

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

/**
 * @brief Builds the environment info block (OS, cwd, local time).
 * @return Newly allocated string owned by the caller, or NULL on OOM.
 */
static char *env_info_build(void)
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

/**
 * @brief Builds the project instructions block from an AGENTS.md file.
 *
 * The file is read once at startup. A missing or unreadable file is a
 * normal condition (no injection, no noise); an empty or whitespace-only
 * file is treated the same way. Content beyond AGENTS_MD_MAX_BYTES is
 * truncated with a warning on stderr. The returned block always ends
 * with a newline and is NUL-terminated.
 *
 * @param path Path of the instructions file (e.g. "AGENTS.md").
 * @return Newly allocated string owned by the caller (free with
 *         free()), or NULL if the file is absent/unreadable/empty or
 *         on allocation failure.
 */
static char *agents_md_build(const char *path)
{
	static const char header[] = AGENTS_MD_HEADER;
	FILE *fp = NULL;
	char *buf = NULL;
	char *out = NULL;
	size_t len = 0;
	size_t i;
	int truncated = 0;

	fp = fopen(path, "rb");
	if (fp == NULL)
		goto cleanup; /* absent or unreadable: no injection */

	/* One extra byte beyond the cap detects truncation, one more
	 * keeps room for the NUL terminator. */
	buf = malloc(AGENTS_MD_MAX_BYTES + 2u);
	if (buf == NULL)
		goto cleanup;

	len = fread(buf, 1, AGENTS_MD_MAX_BYTES + 1u, fp);
	if (ferror(fp))
		goto cleanup;
	if (len > AGENTS_MD_MAX_BYTES) {
		truncated = 1;
		len = AGENTS_MD_MAX_BYTES;
	}
	buf[len] = '\0';

	/* Whitespace-only content carries no instructions: skip it. */
	for (i = 0; i < len; ++i) {
		if (!isspace((unsigned char)buf[i]))
			break;
	}
	if (i == len)
		goto cleanup;

	if (truncated)
		fprintf(stderr,
			"emma: warning: AGENTS.md truncated to %u bytes\n",
			(unsigned)AGENTS_MD_MAX_BYTES);

	{
		size_t extra = (len > 0 && buf[len - 1] != '\n') ? 1u : 0u;
		size_t need = sizeof(header) - 1u + len + extra + 1u;
		size_t off;

		out = malloc(need);
		if (out == NULL)
			goto cleanup;
		memcpy(out, header, sizeof(header) - 1u);
		off = sizeof(header) - 1u;
		memcpy(out + off, buf, len);
		off += len;
		if (extra != 0)
			out[off++] = '\n';
		out[off] = '\0';
	}

cleanup:
	if (fp != NULL)
		fclose(fp);
	free(buf);
	return out;
}

char *system_prompt_full(void)
{
	char *env = NULL;
	char *agents = NULL;
	char *full = NULL;
	size_t len;
	int w;

	env = env_info_build();
	if (env == NULL) {
		/* Graceful degradation: keep going without the block. */
		fprintf(stderr,
			"emma: warning: environment info unavailable\n");
	}
	agents = agents_md_build("AGENTS.md");

	len = strlen(system_prompt()) + (env != NULL ? strlen(env) : 0) +
	      (agents != NULL ? strlen(agents) : 0) + 1u;
	full = malloc(len);
	if (full == NULL)
		goto cleanup;

	w = snprintf(full, len, "%s%s%s", system_prompt(),
		     env != NULL ? env : "", agents != NULL ? agents : "");
	if (w < 0 || (size_t)w >= len) {
		free(full);
		full = NULL;
		goto cleanup;
	}

cleanup:
	free(agents);
	free(env);
	return full;
}
