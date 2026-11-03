/**
 * @file test_system_prompt.c
 * @brief Unit tests for the built-in system prompt module (static
 *        prompt, environment block and AGENTS.md injection).
 */

#include "test.h"
#include "system_prompt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct utsname;
extern char *getcwd(char *buf, size_t size);
extern char *mkdtemp(char *template);
extern int chdir(const char *path);
extern int unlink(const char *path);
extern int rmdir(const char *path);

/* Cap used by agents_md_build(): mirrored here to test truncation. */
#define TEST_AGENTS_MD_MAX (64u * 1024u)

static void test_system_prompt_not_null(void)
{
	TEST_CHECK_NOT_NULL(system_prompt());
}

static void test_system_prompt_not_empty(void)
{
	const char *p = system_prompt();

	TEST_CHECK_NOT_NULL(p);
	TEST_CHECK(p[0] != '\0');
}

static void test_system_prompt_stable_pointer(void)
{
	/* Same pointer on repeated calls: the prompt is a static constant,
	 * which keeps the first request message byte-identical. */
	TEST_CHECK(system_prompt() == system_prompt());
}

static void test_system_prompt_content(void)
{
	const char *p = system_prompt();

	TEST_CHECK_NOT_NULL(strstr(p, "emma"));
	TEST_CHECK_NOT_NULL(strstr(p, "read_file"));
	TEST_CHECK_NOT_NULL(strstr(p, "list_dir"));
	TEST_CHECK_NOT_NULL(strstr(p, "write_file"));
	TEST_CHECK_NOT_NULL(strstr(p, "bash"));
	/* No trailing whitespace: the prompt must be byte-stable. */
	TEST_CHECK(p[strlen(p) - 1] != '\n');
	TEST_CHECK(p[strlen(p) - 1] != ' ');
}

static void test_system_prompt_workflow(void)
{
	const char *p = system_prompt();

	/* Explicit numbered workflow for non-reasoning models. */
	TEST_CHECK_NOT_NULL(strstr(p, "Workflow"));
	TEST_CHECK_NOT_NULL(strstr(p, "1. EXPLORE"));
	TEST_CHECK_NOT_NULL(strstr(p, "2. LOCATE"));
	TEST_CHECK_NOT_NULL(strstr(p, "3. CHANGE"));
	TEST_CHECK_NOT_NULL(strstr(p, "4. VERIFY"));
}

static void test_system_prompt_hard_rules(void)
{
	const char *p = system_prompt();

	/* Categorical prohibitions. */
	TEST_CHECK_NOT_NULL(strstr(p, "Hard rules:"));
	TEST_CHECK_NOT_NULL(strstr(p, "NEVER answer a question about the code"));
	TEST_CHECK_NOT_NULL(strstr(p, "NEVER invent file names"));
	TEST_CHECK_NOT_NULL(strstr(p, "NEVER create or overwrite a file"));
	/* Tool calls must be executed, not narrated in prose. */
	TEST_CHECK_NOT_NULL(strstr(p, "NEVER describe or narrate tool calls"));
	TEST_CHECK_NOT_NULL(strstr(p, "Keep prose short"));
}

static void test_system_prompt_full_prefix(void)
{
	char *full = system_prompt_full();
	const char *base = system_prompt();

	TEST_CHECK_NOT_NULL(full);
	if (full != NULL) {
		/* The full prompt starts with the exact static prompt. */
		TEST_CHECK(strncmp(full, base, strlen(base)) == 0);
	}
	free(full);
}

static void test_system_prompt_full_has_env_block(void)
{
	char *full = system_prompt_full();

	TEST_CHECK_NOT_NULL(full);
	if (full != NULL) {
		TEST_CHECK_NOT_NULL(strstr(full, "Environment:"));
		TEST_CHECK_NOT_NULL(strstr(full, "- OS: "));
		TEST_CHECK_NOT_NULL(strstr(full, "- Working directory: "));
		TEST_CHECK_NOT_NULL(strstr(full, "- Date: "));
	}
	free(full);
}

static void test_system_prompt_full_cwd_matches(void)
{
	char *full = system_prompt_full();
	char cwd[4096];

	TEST_CHECK_NOT_NULL(full);
	if (getcwd(cwd, sizeof(cwd)) != NULL) {
		char line[4200];

		snprintf(line, sizeof(line), "- Working directory: %s\n", cwd);
		TEST_CHECK_NOT_NULL(strstr(full, line));
	}
	free(full);
}

static void test_system_prompt_full_date_format(void)
{
	char *full = system_prompt_full();
	const char *date;

	TEST_CHECK_NOT_NULL(full);
	date = strstr(full, "- Date: ");
	TEST_CHECK_NOT_NULL(date);
	if (date != NULL) {
		int y, mon, d, h, min;

		/* Expected: "- Date: YYYY-MM-DD HH:MM (local time)" */
		TEST_CHECK_INT_EQ(sscanf(date,
					 "- Date: %d-%d-%d %d:%d",
					 &y, &mon, &d, &h, &min), 5);
		TEST_CHECK_NOT_NULL(strstr(date, "(local time)"));
	}
	free(full);
}

static void test_system_prompt_full_nul_terminated(void)
{
	char *full = system_prompt_full();

	TEST_CHECK_NOT_NULL(full);
	if (full != NULL) {
		size_t len = strlen(full);

		/* Reachable NUL and sane trailing newline. */
		TEST_CHECK(len > 0);
		TEST_CHECK(full[len] == '\0');
		TEST_CHECK(full[len - 1] == '\n');
	}
	free(full);
}

static void test_system_prompt_full_independent_allocations(void)
{
	char *a = system_prompt_full();
	char *b = system_prompt_full();

	TEST_CHECK_NOT_NULL(a);
	TEST_CHECK_NOT_NULL(b);
	/* Distinct heap blocks: no aliasing, no double-free risk. */
	TEST_CHECK(a != b);
	TEST_CHECK_STR_EQ(a, b);
	free(a);
	free(b);
}

/* --- AGENTS.md injection ------------------------------------------- */

/* Creates a unique temporary directory; returns a heap string owned by
 * the caller, or NULL on failure. */
static char *test_temp_dir(void)
{
	const char *tmp = getenv("TMPDIR");
	size_t need;
	char *tmpl;

	if (tmp == NULL || tmp[0] == '\0')
		tmp = "/tmp";
	need = strlen(tmp) + sizeof("/emma_agents_XXXXXX");
	tmpl = malloc(need);
	if (tmpl == NULL)
		return NULL;
	snprintf(tmpl, need, "%s/emma_agents_XXXXXX", tmp);
	if (mkdtemp(tmpl) == NULL) {
		free(tmpl);
		return NULL;
	}
	return tmpl;
}

/* Heap copy of a string (tests do not link util). */
static char *test_strdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = malloc(n);

	if (p != NULL)
		memcpy(p, s, n);
	return p;
}

/* Saves the current working directory as a heap string, or NULL. */
static char *test_save_cwd(void)
{
	char buf[4096];

	if (getcwd(buf, sizeof(buf)) == NULL)
		return NULL;
	return test_strdup(buf);
}

static int test_write_file(const char *path, const char *content, size_t len)
{
	FILE *fp = fopen(path, "wb");
	int rc = -1;

	if (fp == NULL)
		return -1;
	if (fwrite(content, 1, len, fp) == len)
		rc = 0;
	fclose(fp);
	return rc;
}

static void test_full_without_agents_md(void)
{
	char *old_cwd = NULL;
	char *dir = NULL;
	char *full = NULL;
	const char *base = system_prompt();

	old_cwd = test_save_cwd();
	dir = test_temp_dir();
	TEST_CHECK_NOT_NULL(old_cwd);
	TEST_CHECK_NOT_NULL(dir);
	if (old_cwd == NULL || dir == NULL)
		goto cleanup;

	TEST_CHECK(chdir(dir) == 0);
	full = system_prompt_full();
	TEST_CHECK_NOT_NULL(full);
	if (full != NULL) {
		/* Static prompt is still the exact prefix... */
		TEST_CHECK(strncmp(full, base, strlen(base)) == 0);
		/* ...and no instructions block was injected. */
		TEST_CHECK(strstr(full,
			"Project instructions (AGENTS.md)") == NULL);
	}

cleanup:
	if (full != NULL) {
		free(full);
		full = NULL;
	}
	if (old_cwd != NULL && chdir(old_cwd) != 0)
		fprintf(stderr, "test: cannot restore cwd\n");
	rmdir(dir);
	free(dir);
	free(old_cwd);
}

static void test_full_with_agents_md(void)
{
	static const char content[] =
		"Be terse.\nRespect the Makefile.\n";
	char *old_cwd = NULL;
	char *dir = NULL;
	char *full = NULL;
	char path[4200];
	const char *base = system_prompt();

	old_cwd = test_save_cwd();
	dir = test_temp_dir();
	TEST_CHECK_NOT_NULL(old_cwd);
	TEST_CHECK_NOT_NULL(dir);
	if (old_cwd == NULL || dir == NULL)
		goto cleanup;

	TEST_CHECK(chdir(dir) == 0);
	snprintf(path, sizeof(path), "%s/AGENTS.md", dir);
	TEST_CHECK(test_write_file(path, content, strlen(content)) == 0);

	full = system_prompt_full();
	TEST_CHECK_NOT_NULL(full);
	if (full != NULL) {
		/* Static prompt prefix is untouched... */
		TEST_CHECK(strncmp(full, base, strlen(base)) == 0);
		/* ...and the block carries the exact file content. */
		TEST_CHECK_NOT_NULL(strstr(full,
			"\n\nProject instructions (AGENTS.md):\n\n"
			"Be terse.\nRespect the Makefile.\n"));
	}

cleanup:
	if (full != NULL) {
		free(full);
		full = NULL;
	}
	if (old_cwd != NULL && chdir(old_cwd) != 0)
		fprintf(stderr, "test: cannot restore cwd\n");
	unlink(path);
	rmdir(dir);
	free(dir);
	free(old_cwd);
}

static void test_full_with_empty_agents_md(void)
{
	static const char content[] = "   \n\t\n";
	char *old_cwd = NULL;
	char *dir = NULL;
	char *full = NULL;
	char path[4200];

	old_cwd = test_save_cwd();
	dir = test_temp_dir();
	TEST_CHECK_NOT_NULL(old_cwd);
	TEST_CHECK_NOT_NULL(dir);
	if (old_cwd == NULL || dir == NULL)
		goto cleanup;

	TEST_CHECK(chdir(dir) == 0);
	snprintf(path, sizeof(path), "%s/AGENTS.md", dir);
	TEST_CHECK(test_write_file(path, content, strlen(content)) == 0);

	full = system_prompt_full();
	TEST_CHECK_NOT_NULL(full);
	if (full != NULL) {
		/* Whitespace-only file: no instructions block. */
		TEST_CHECK(strstr(full,
			"Project instructions (AGENTS.md)") == NULL);
	}

cleanup:
	if (full != NULL) {
		free(full);
		full = NULL;
	}
	if (old_cwd != NULL && chdir(old_cwd) != 0)
		fprintf(stderr, "test: cannot restore cwd\n");
	unlink(path);
	rmdir(dir);
	free(dir);
	free(old_cwd);
}

static void test_full_agents_md_truncated(void)
{
	size_t n = 70u * 1024u;
	char *big = NULL;
	char *old_cwd = NULL;
	char *dir = NULL;
	char *full = NULL;
	char path[4200];

	big = malloc(n);
	TEST_CHECK_NOT_NULL(big);
	if (big == NULL)
		return;
	memset(big, 'a', n);

	old_cwd = test_save_cwd();
	dir = test_temp_dir();
	TEST_CHECK_NOT_NULL(old_cwd);
	TEST_CHECK_NOT_NULL(dir);
	if (old_cwd == NULL || dir == NULL)
		goto cleanup;

	TEST_CHECK(chdir(dir) == 0);
	snprintf(path, sizeof(path), "%s/AGENTS.md", dir);
	TEST_CHECK(test_write_file(path, big, n) == 0);

	full = system_prompt_full();
	TEST_CHECK_NOT_NULL(full);
	if (full != NULL) {
		size_t len = strlen(full);

		TEST_CHECK_NOT_NULL(strstr(full,
			"Project instructions (AGENTS.md):"));
		/* Capped at AGENTS_MD_MAX content bytes, plus the
		 * appended trailing newline, plus prompt and env. */
		TEST_CHECK(len >= TEST_AGENTS_MD_MAX);
		TEST_CHECK(len < TEST_AGENTS_MD_MAX + 8192u +
			   strlen(system_prompt()));
		/* Truncation happened mid-content: the block ends with
		 * the last 'a' of the capped content, newline-terminated. */
		TEST_CHECK(full[len - 1] == '\n');
		TEST_CHECK(full[len - 2] == 'a');
	}

cleanup:
	free(full);
	if (old_cwd != NULL && chdir(old_cwd) != 0)
		fprintf(stderr, "test: cannot restore cwd\n");
	unlink(path);
	rmdir(dir);
	free(dir);
	free(old_cwd);
	free(big);
}

int main(void)
{
	test_system_prompt_not_null();
	test_system_prompt_not_empty();
	test_system_prompt_stable_pointer();
	test_system_prompt_content();
	test_system_prompt_workflow();
	test_system_prompt_hard_rules();
	test_system_prompt_full_prefix();
	test_system_prompt_full_has_env_block();
	test_system_prompt_full_cwd_matches();
	test_system_prompt_full_date_format();
	test_system_prompt_full_nul_terminated();
	test_system_prompt_full_independent_allocations();
	test_full_without_agents_md();
	test_full_with_agents_md();
	test_full_with_empty_agents_md();
	test_full_agents_md_truncated();

	return test__failures != 0;
}
