/**
 * @file test_tools.c
 * @brief Unit tests for src/tools.c: registry behavior and the built-in
 *        tool handlers (read_file, write_file, bash).
 */

#include "test.h"
#include "tools.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed scratch path used by the file tool tests (removed before and
 * after each use). */
#define TMP_FILE "/tmp/emma_tools_test.txt"

static void remove_tmp_file(void)
{
	remove(TMP_FILE);
}

static void test_registry_init_and_free(void)
{
	tool_registry_t *reg = NULL;

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_NOT_NULL(reg);
	TEST_CHECK_INT_EQ((long long)tools_count(reg), 0);
	TEST_CHECK_NULL(tools_find(reg, "read_file"));
	TEST_CHECK_NULL(tools_at(reg, 0));
	tools_registry_free(reg);

	/* NULL safety. */
	tools_registry_free(NULL);
	TEST_CHECK_INT_EQ((long long)tools_registry_init(NULL), -1);
	TEST_CHECK_INT_EQ((long long)tools_count(NULL), 0);
	TEST_CHECK_NULL(tools_find(NULL, "x"));
	TEST_CHECK_NULL(tools_at(NULL, 0));
}

/** Trivial handler used by the registry tests. */
static int fake_handler(const tool_def_t *tool, const char *args_json,
			char **result)
{
	(void)tool;
	(void)args_json;
	*result = util_strdup("fake-ok");
	return 0;
}

/** Trivial title formatter used by the registry tests. */
static char *fake_title(const tool_def_t *tool, const char *args_json)
{
	(void)tool;
	(void)args_json;
	return util_strdup("fake-title");
}

static void test_title_callback(void)
{
	tool_registry_t *reg = NULL;
	const tool_def_t *def;
	char *title = NULL;

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);

	/* NULL title is accepted and stored as NULL (generic fallback). */
	TEST_CHECK_INT_EQ((long long)tools_register(reg, "plain", "d", "{}",
					    0, fake_handler, NULL, NULL), 0);
	def = tools_find(reg, "plain");
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_NULL(def->title);

	/* A provided title callback is stored and callable. */
	TEST_CHECK_INT_EQ((long long)tools_register(reg, "titled", "d", "{}",
					    0, fake_handler, fake_title,
					    NULL), 0);
	def = tools_find(reg, "titled");
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_NOT_NULL(def->title);
	title = def->title(def, "{}");
	TEST_CHECK_STR_EQ(title, "fake-title");
	free(title);

	tools_registry_free(reg);
}

static void test_builtin_titles(void)
{
	tool_registry_t *reg = NULL;
	const tool_def_t *def;
	char *title = NULL;

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_register_builtins(reg), 0);

	/* bash: the title is the tool name + command. */
	def = tools_find(reg, "bash");
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_NOT_NULL(def->title);
	title = def->title(def, "{\"command\":\"ls -la\"}");
	TEST_CHECK_STR_EQ(title, "bash ls -la");
	free(title);

	/* read_file: the title is the tool name + path. */
	def = tools_find(reg, "read_file");
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_NOT_NULL(def->title);
	title = def->title(def, "{\"path\":\"/tmp/x\"}");
	TEST_CHECK_STR_EQ(title, "read_file /tmp/x");
	free(title);

	/* write_file: the title is the tool name + path too. */
	def = tools_find(reg, "write_file");
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_NOT_NULL(def->title);
	title = def->title(def,
			   "{\"path\":\"/tmp/y\",\"content\":\"c\"}");
	TEST_CHECK_STR_EQ(title, "write_file /tmp/y");
	free(title);

	/* Malformed arguments yield NULL (the dispatcher falls back to
	 * the generic "name: preview" format). */
	TEST_CHECK_NULL(def->title(def, NULL));
	TEST_CHECK_NULL(def->title(def, "not json"));
	TEST_CHECK_NULL(def->title(def, "{}"));

	tools_registry_free(reg);
}

static void test_register_and_find(void)
{
	tool_registry_t *reg = NULL;
	const tool_def_t *def;

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_register(reg, "alpha", "desc A",
						    "{}", 0, fake_handler, NULL,
						    NULL), 0);
	TEST_CHECK_INT_EQ((long long)tools_register(reg, "beta", "desc B",
						    "{}", 1, fake_handler, NULL,
						    NULL), 0);
	TEST_CHECK_INT_EQ((long long)tools_count(reg), 2);

	def = tools_find(reg, "alpha");
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_STR_EQ(def->description, "desc A");
	TEST_CHECK_INT_EQ((long long)def->requires_approval, 0);
	TEST_CHECK_NOT_NULL(def->handler);
	TEST_CHECK_STR_EQ(def->parameters, "{}");

	def = tools_find(reg, "beta");
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_INT_EQ((long long)def->requires_approval, 1);

	def = tools_at(reg, 1);
	TEST_CHECK_NOT_NULL(def);
	TEST_CHECK_STR_EQ(def->name, "beta");
	TEST_CHECK_NULL(tools_at(reg, 2));
	TEST_CHECK_NULL(tools_find(reg, "gamma"));

	tools_registry_free(reg);
}

static void test_register_invalid_args(void)
{
	tool_registry_t *reg = NULL;

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_register(NULL, "x", "d", "{}", 0,
						    fake_handler, NULL, NULL), -1);
	TEST_CHECK_INT_EQ((long long)tools_register(reg, NULL, "d", "{}", 0,
						    fake_handler, NULL, NULL), -1);
	TEST_CHECK_INT_EQ((long long)tools_register(reg, "", "d", "{}", 0,
						    fake_handler, NULL, NULL), -1);
	TEST_CHECK_INT_EQ((long long)tools_register(reg, "x", NULL, "{}", 0,
						    fake_handler, NULL, NULL), -1);
	TEST_CHECK_INT_EQ((long long)tools_register(reg, "x", "d", NULL, 0,
						    fake_handler, NULL, NULL), -1);
	TEST_CHECK_INT_EQ((long long)tools_register(reg, "x", "d", "{}", 0,
						    NULL, NULL, NULL), -1);
	TEST_CHECK_INT_EQ((long long)tools_count(reg), 0);

	/* Duplicate names are rejected. */
	TEST_CHECK_INT_EQ((long long)tools_register(reg, "dup", "d", "{}", 0,
						    fake_handler, NULL, NULL), 0);
	TEST_CHECK_INT_EQ((long long)tools_register(reg, "dup", "other", "{}",
						    0, fake_handler, NULL, NULL),
			  -1);
	TEST_CHECK_INT_EQ((long long)tools_count(reg), 1);

	tools_registry_free(reg);
}

static void test_build_json(void)
{
	tool_registry_t *reg = NULL;
	char *json;

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_NULL(tools_build_json(NULL));
	TEST_CHECK_NULL(tools_build_json(reg)); /* empty registry */

	TEST_CHECK_INT_EQ((long long)tools_register(reg, "my_tool",
						    "does things",
						    "{\"type\":\"object\"}", 0,
						    fake_handler, NULL, NULL), 0);
	json = tools_build_json(reg);
	TEST_CHECK_NOT_NULL(json);
	TEST_CHECK(strstr(json, "\"type\":\"function\"") != NULL);
	TEST_CHECK(strstr(json, "\"name\":\"my_tool\"") != NULL);
	TEST_CHECK(strstr(json, "\"description\":\"does things\"") != NULL);
	TEST_CHECK(strstr(json, "\"parameters\":{\"type\":\"object\"}") !=
		   NULL);
	free(json);

	tools_registry_free(reg);
}

static void test_builtins_registered(void)
{
	tool_registry_t *reg = NULL;
	const char *names[] = { "read_file", "write_file", "bash" };

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_register_builtins(reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_count(reg), 3);
	for (size_t i = 0; i < 3; ++i) {
		const tool_def_t *def = tools_find(reg, names[i]);

		TEST_CHECK_NOT_NULL(def);
		TEST_CHECK_NOT_NULL(def->description);
		TEST_CHECK_NOT_NULL(def->parameters);
		TEST_CHECK_NOT_NULL(def->handler);
	}
	/* Only bash requires approval. */
	TEST_CHECK_INT_EQ((long long)tools_find(reg, "bash")->requires_approval,
			  1);
	TEST_CHECK_INT_EQ(
	    (long long)tools_find(reg, "read_file")->requires_approval, 0);
	TEST_CHECK_INT_EQ(
	    (long long)tools_find(reg, "write_file")->requires_approval, 0);
	/* list_dir was removed: it must no longer be registered. */
	TEST_CHECK(tools_find(reg, "list_dir") == NULL);
	TEST_CHECK_INT_EQ((long long)tools_register_builtins(NULL), -1);

	tools_registry_free(reg);
}

static void test_read_file_success(void)
{
	tool_registry_t *reg = NULL;
	const tool_def_t *def;
	char *result = NULL;

	remove_tmp_file();
	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_register_builtins(reg), 0);
	def = tools_find(reg, "read_file");

	FILE *fp = fopen(TMP_FILE, "wb");

	if (fp != NULL) {
		fputs("line one\nline two\n", fp);
		fclose(fp);
	}
	TEST_CHECK_NOT_NULL(fp);

	TEST_CHECK_INT_EQ((long long)def->handler(
			      def, "{\"path\":\"" TMP_FILE "\"}", &result),
			  0);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK_STR_EQ(result, "line one\nline two\n");
	free(result);
	remove_tmp_file();
	tools_registry_free(reg);
}

static void test_read_file_errors(void)
{
	tool_registry_t *reg = NULL;
	const tool_def_t *def;
	char *result = NULL;

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_register_builtins(reg), 0);
	def = tools_find(reg, "read_file");

	/* Missing file. */
	TEST_CHECK_INT_EQ(
	    (long long)def->handler(
		def, "{\"path\":\"/tmp/emma_no_such_file_42\"}", &result),
	    -1);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK(strstr(result, "cannot open") != NULL);
	free(result);

	/* Malformed JSON arguments. */
	TEST_CHECK_INT_EQ((long long)def->handler(def, "not json", &result),
			  -1);
	TEST_CHECK_NOT_NULL(result);
	free(result);

	/* Missing 'path' field. */
	TEST_CHECK_INT_EQ((long long)def->handler(def, "{}", &result), -1);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK(strstr(result, "path") != NULL);
	free(result);

	/* Empty arguments. */
	TEST_CHECK_INT_EQ((long long)def->handler(def, "", &result), -1);
	TEST_CHECK_NOT_NULL(result);
	free(result);

	tools_registry_free(reg);
}

static void test_write_file_success(void)
{
	tool_registry_t *reg = NULL;
	const tool_def_t *def;
	char *result = NULL;

	remove_tmp_file();
	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_register_builtins(reg), 0);
	def = tools_find(reg, "write_file");

	TEST_CHECK_INT_EQ(
	    (long long)def->handler(def, "{\"path\":\"" TMP_FILE
					 "\",\"content\":\"hello\\nworld\"}",
				    &result),
	    0);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK(strstr(result, "wrote 11 bytes") != NULL);
	free(result);

	/* Read back through the read_file tool to prove the round-trip. */
	def = tools_find(reg, "read_file");
	TEST_CHECK_INT_EQ((long long)def->handler(
			      def, "{\"path\":\"" TMP_FILE "\"}", &result),
			  0);
	TEST_CHECK_STR_EQ(result, "hello\nworld");
	free(result);

	remove_tmp_file();
	tools_registry_free(reg);
}

static void test_write_file_errors(void)
{
	tool_registry_t *reg = NULL;
	const tool_def_t *def;
	char *result = NULL;

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_register_builtins(reg), 0);
	def = tools_find(reg, "write_file");

	/* Missing content field. */
	TEST_CHECK_INT_EQ((long long)def->handler(
			      def, "{\"path\":\"" TMP_FILE "\"}", &result),
			  -1);
	TEST_CHECK_NOT_NULL(result);
	free(result);

	/* Unwritable path. */
	TEST_CHECK_INT_EQ(
	    (long long)def->handler(
		def,
		"{\"path\":\"/tmp/emma_no_dir_42/sub/file\",\"content\":\"x\"}",
		&result),
	    -1);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK(strstr(result, "cannot") != NULL);
	free(result);

	tools_registry_free(reg);
}

static void test_bash(void)
{
	tool_registry_t *reg = NULL;
	const tool_def_t *def;
	char *result = NULL;

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_register_builtins(reg), 0);
	def = tools_find(reg, "bash");

	/* Successful command. */
	TEST_CHECK_INT_EQ(
	    (long long)def->handler(def, "{\"command\":\"echo hello-bash\"}",
				    &result),
	    0);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK(strstr(result, "hello-bash") != NULL);
	TEST_CHECK(strstr(result, "exit status") != NULL);
	free(result);

	/* Failing command: nonzero exit is reported but not fatal. */
	TEST_CHECK_INT_EQ(
	    (long long)def->handler(def, "{\"command\":\"exit 3\"}", &result),
	    -1);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK(strstr(result, "exit status: 3") != NULL);
	free(result);

	/* Empty command. */
	TEST_CHECK_INT_EQ((long long)def->handler(def, "{\"command\":\"\"}",
						  &result),
			  -1);
	TEST_CHECK_NOT_NULL(result);
	free(result);

	/* Malformed arguments. */
	TEST_CHECK_INT_EQ((long long)def->handler(def, "{\"cmd\":\"x\"}",
						  &result),
			  -1);
	TEST_CHECK_NOT_NULL(result);
	free(result);

	tools_registry_free(reg);
}

static void test_bash_output_truncated(void)
{
	tool_registry_t *reg = NULL;
	const tool_def_t *def;
	char *result = NULL;

	TEST_CHECK_INT_EQ((long long)tools_registry_init(&reg), 0);
	TEST_CHECK_INT_EQ((long long)tools_register_builtins(reg), 0);
	def = tools_find(reg, "bash");

	TEST_CHECK_INT_EQ(
	    (long long)def->handler(
		def, "{\"command\":\"yes | head -c 200000\"}", &result),
	    0);
	TEST_CHECK_NOT_NULL(result);
	TEST_CHECK(strlen(result) <= (size_t)TOOLS_RESULT_MAX);
	TEST_CHECK(strstr(result, "[output truncated]") != NULL);
	free(result);

	tools_registry_free(reg);
}

int main(void)
{
	TEST_RUN(test_title_callback);
	TEST_RUN(test_builtin_titles);
	TEST_RUN(test_registry_init_and_free);
	TEST_RUN(test_register_and_find);
	TEST_RUN(test_register_invalid_args);
	TEST_RUN(test_build_json);
	TEST_RUN(test_builtins_registered);
	TEST_RUN(test_read_file_success);
	TEST_RUN(test_read_file_errors);
	TEST_RUN(test_write_file_success);
	TEST_RUN(test_write_file_errors);
	TEST_RUN(test_bash);
	TEST_RUN(test_bash_output_truncated);
	return test__failures != 0;
}
