/**
 * @file test_toml.c
 * @brief Unit tests for src/toml.c (pure parsing, no network).
 */

#include "test.h"
#include "toml.h"

#include <stdio.h>
#include <string.h>

static void test_parse_scalars(void)
{
	toml_table_t *t = NULL;
	const char *s = NULL;
	long long i = 0;
	double d = 0.0;
	int b = -1;

	TEST_CHECK_INT_EQ(toml_parse_string(
		"str = \"a\\\"b\\\\c\\nd\\te\"\n"
		"lit = 'x \\t y'\n"
		"i1 = 42\n"
		"i2 = -7\n"
		"i3 = +7\n"
		"i4 = 1_000\n"
		"f1 = 3.14\n"
		"f2 = -0.5\n"
		"f3 = 1e5\n"
		"b1 = true\n"
		"b2 = false\n",
		&t, NULL, 0), 0);

	TEST_CHECK_INT_EQ(toml_get_string(t, "str", &s), 0);
	TEST_CHECK_STR_EQ(s, "a\"b\\c\nd\te");
	TEST_CHECK_INT_EQ(toml_get_string(t, "lit", &s), 0);
	TEST_CHECK_STR_EQ(s, "x \\t y");
	TEST_CHECK_INT_EQ(toml_get_int(t, "i1", &i), 0);
	TEST_CHECK_INT_EQ(i, 42);
	TEST_CHECK_INT_EQ(toml_get_int(t, "i2", &i), 0);
	TEST_CHECK_INT_EQ(i, -7);
	TEST_CHECK_INT_EQ(toml_get_int(t, "i3", &i), 0);
	TEST_CHECK_INT_EQ(i, 7);
	TEST_CHECK_INT_EQ(toml_get_int(t, "i4", &i), 0);
	TEST_CHECK_INT_EQ(i, 1000);
	TEST_CHECK_INT_EQ(toml_get_double(t, "f1", &d), 0);
	TEST_CHECK(d == 3.14);
	TEST_CHECK_INT_EQ(toml_get_double(t, "f2", &d), 0);
	TEST_CHECK(d == -0.5);
	TEST_CHECK_INT_EQ(toml_get_double(t, "f3", &d), 0);
	TEST_CHECK(d == 100000.0);
	TEST_CHECK_INT_EQ(toml_get_bool(t, "b1", &b), 0);
	TEST_CHECK_INT_EQ(b, 1);
	TEST_CHECK_INT_EQ(toml_get_bool(t, "b2", &b), 0);
	TEST_CHECK_INT_EQ(b, 0);

	toml_free(t);
}

static void test_parse_tables_and_dotted_keys(void)
{
	toml_table_t *t = NULL;
	const char *s = NULL;
	long long i = 0;
	toml_type_t type = TOML_STRING;

	TEST_CHECK_INT_EQ(toml_parse_string(
		"x.y.z = \"deep\"\n"
		"n = 3\n"
		"[server]\n"
		"host = \"a\"\n"
		"[server.tls]\n"
		"enabled = true\n",
		&t, NULL, 0), 0);

	TEST_CHECK_INT_EQ(toml_get_string(t, "server.host", &s), 0);
	TEST_CHECK_STR_EQ(s, "a");
	{
		int flag = 0;

		TEST_CHECK_INT_EQ(toml_get_bool(t, "server.tls.enabled",
						&flag), 0);
		TEST_CHECK_INT_EQ(flag, 1);
	}
	TEST_CHECK_INT_EQ(toml_get_string(t, "x.y.z", &s), 0);
	TEST_CHECK_STR_EQ(s, "deep");
	TEST_CHECK_INT_EQ(toml_get_int(t, "n", &i), 0);
	TEST_CHECK_INT_EQ(i, 3);

	/* Table lookup through toml_get(). */
	TEST_CHECK(toml_get(t, "server.tls", &type) != NULL);
	TEST_CHECK_INT_EQ((long long)type, (long long)TOML_TABLE);

	toml_free(t);
}

static void test_parse_arrays(void)
{
	toml_table_t *t = NULL;
	toml_array_t *arr = NULL;
	const void *item = NULL;
	toml_type_t type = TOML_TABLE;
	long long i = 0;

	TEST_CHECK_INT_EQ(toml_parse_string(
		"mixed = [1, 2.5, \"x\", true]\n"
		"trailing = [1, 2,]\n"
		"multiline = [\n 1,\n 2,\n]\n"
		"empty = []\n",
		&t, NULL, 0), 0);

	TEST_CHECK(toml_get(t, "mixed", &type) != NULL);
	TEST_CHECK_INT_EQ((long long)type, (long long)TOML_ARRAY);
	arr = (toml_array_t *)toml_get(t, "mixed", NULL);
	TEST_CHECK_NOT_NULL(arr);
	TEST_CHECK_INT_EQ((long long)toml_array_count(arr), 4);

	item = toml_array_item(arr, 0, &type);
	TEST_CHECK_NOT_NULL(item);
	TEST_CHECK_INT_EQ((long long)type, (long long)TOML_INT);
	TEST_CHECK_INT_EQ(*(const long long *)item, 1);

	item = toml_array_item(arr, 1, &type);
	TEST_CHECK_NOT_NULL(item);
	TEST_CHECK_INT_EQ((long long)type, (long long)TOML_DOUBLE);
	TEST_CHECK(*(const double *)item == 2.5);

	item = toml_array_item(arr, 2, &type);
	TEST_CHECK_NOT_NULL(item);
	TEST_CHECK_INT_EQ((long long)type, (long long)TOML_STRING);
	TEST_CHECK_STR_EQ(item, "x");

	item = toml_array_item(arr, 3, &type);
	TEST_CHECK_NOT_NULL(item);
	TEST_CHECK_INT_EQ((long long)type, (long long)TOML_BOOL);
	TEST_CHECK_INT_EQ(*(const int *)item, 1);

	TEST_CHECK_NULL(toml_array_item(arr, 4, &type));

	arr = (toml_array_t *)toml_get(t, "trailing", NULL);
	TEST_CHECK_INT_EQ((long long)toml_array_count(arr), 2);

	arr = (toml_array_t *)toml_get(t, "multiline", NULL);
	TEST_CHECK_INT_EQ((long long)toml_array_count(arr), 2);

	arr = (toml_array_t *)toml_get(t, "empty", NULL);
	TEST_CHECK_INT_EQ((long long)toml_array_count(arr), 0);

	TEST_CHECK_INT_EQ(toml_get_int(t, "x", &i), -1); /* untouched */

	toml_free(t);
}

static void test_parse_comments_and_whitespace(void)
{
	toml_table_t *t = NULL;
	const char *s = NULL;

	TEST_CHECK_INT_EQ(toml_parse_string(
		"# leading comment\n"
		"\n"
		"   \n"
		"k = \"v\"   # trailing comment\n"
		"# another\n",
		&t, NULL, 0), 0);
	TEST_CHECK_INT_EQ(toml_get_string(t, "k", &s), 0);
	TEST_CHECK_STR_EQ(s, "v");
	toml_free(t);

	/* No trailing newline on the last line. */
	TEST_CHECK_INT_EQ(toml_parse_string("k = 'v'", &t, NULL, 0), 0);
	TEST_CHECK_INT_EQ(toml_get_string(t, "k", &s), 0);
	TEST_CHECK_STR_EQ(s, "v");
	toml_free(t);
}

static void test_parse_empty(void)
{
	toml_table_t *t = NULL;
	toml_type_t type = TOML_STRING;

	TEST_CHECK_INT_EQ(toml_parse_string("", &t, NULL, 0), 0);
	TEST_CHECK_NOT_NULL(t);
	TEST_CHECK_NULL(toml_get(t, "missing", &type));
	TEST_CHECK_INT_EQ(toml_get_int(t, "missing", NULL), -1);
	toml_free(t);

	/* Comments-only input parses to an empty table. */
	TEST_CHECK_INT_EQ(toml_parse_string("# only a comment\n", &t,
					    NULL, 0), 0);
	TEST_CHECK_NOT_NULL(t);
	toml_free(t);
}

static void test_parse_rejects_invalid(void)
{
	struct {
		const char *name;
		const char *src;
	} cases[] = {
		{ "duplicate key", "k = 1\nk = 2\n" },
		{ "duplicate table", "[a]\n[a]\n" },
		{ "unterminated string", "k = \"abc\n" },
		{ "unterminated literal", "k = 'abc\n" },
		{ "unterminated array", "k = [1, 2\n" },
		{ "bad escape", "k = \"a\\qb\"\n" },
		{ "truncated unicode escape", "k = \"\\u12\"\n" },
		{ "surrogate escape", "k = \"\\uD800\"\n" },
		{ "trailing garbage", "k = 1 junk\n" },
		{ "inline table", "k = { a = 1 }\n" },
		{ "multiline string", "k = \"\"\"abc\"\"\"\n" },
		{ "array of tables", "[[a]]\n" },
		{ "datetime", "k = 1979-05-27T07:32:00Z\n" },
		{ "time value", "k = 07:32:00\n" },
		{ "hex integer", "k = 0x1F\n" },
		{ "bare inf", "k = inf\n" },
		{ "integer overflow", "k = 99999999999999999999\n" },
		{ "missing value", "k =\n" },
		{ "missing equals", "k 1\n" },
		{ "missing key", "= 1\n" },
		{ "unclosed header", "[a\n" },
		{ "header trailing garbage", "[a] junk\n" },
		{ "nested arrays", "k = [[1]]\n" },
		{ "key into scalar", "a = 1\na.b = 2\n" },
		{ "dotted key into header scalar", "[a]\nb = 1\nb.c = 2\n" },
		{ "empty key path", "a. = 1\n" },
	};
	size_t k;

	for (k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		toml_table_t *t = NULL;
		char errbuf[64];

		errbuf[0] = '\0';
		TEST_CHECK_INT_EQ(toml_parse_string(cases[k].src, &t,
					    errbuf,
					    sizeof errbuf), -1);
		TEST_CHECK_NULL(t);
		TEST_CHECK(errbuf[0] != '\0');
	}
}

static void test_errbuf_truncation(void)
{
	toml_table_t *t = NULL;
	char errbuf[8];

	/* A key of 130 chars exceeds TOML_MAX_KEY (128 incl. NUL): the
	 * resulting message is longer than the 8-byte buffer, which must
	 * stay NUL-terminated. */
	TEST_CHECK_INT_EQ(toml_parse_string(
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		"aaaaaaaaaaaaaaaaaa = 1\n",
		&t, errbuf, sizeof errbuf), -1);
	TEST_CHECK_INT_EQ((long long)strlen(errbuf), 7);
	TEST_CHECK_STR_EQ(errbuf, "line 1:");
}

static void test_parse_file_paths(void)
{
	toml_table_t *t = NULL;
	char errbuf[128];
	FILE *f = NULL;
	const char *path = "build/toml_test_fixture.toml";
	const char *s = NULL;
	const char *good = "base_url = \"http://x\"\nretries = 2\n";

	/* Missing file: clear message, no crash. */
	errbuf[0] = '\0';
	TEST_CHECK_INT_EQ(toml_parse_file("build/no_such_file.toml", &t,
					  errbuf, sizeof errbuf), -1);
	TEST_CHECK_NOT_NULL(strstr(errbuf, "build/no_such_file.toml"));

	f = fopen(path, "w");
	TEST_CHECK_NOT_NULL(f);
	if (f == NULL)
		return;
	if (fwrite(good, 1, strlen(good), f) != strlen(good))
		TEST_CHECK(0);
	fclose(f);

	TEST_CHECK_INT_EQ(toml_parse_file(path, &t, errbuf,
					  sizeof errbuf), 0);
	TEST_CHECK_INT_EQ(toml_get_string(t, "base_url", &s), 0);
	TEST_CHECK_STR_EQ(s, "http://x");
	{
		long long retries = 0;

		TEST_CHECK_INT_EQ(toml_get_int(t, "retries", &retries), 0);
		TEST_CHECK_INT_EQ(retries, 2);
	}
	toml_free(t);

	remove(path);
}

static void test_null_safety(void)
{
	toml_table_t *t = NULL;
	toml_type_t type = TOML_STRING;
	const char *s = NULL;
	long long i = 0;

	TEST_CHECK_INT_EQ(toml_parse_string(NULL, &t, NULL, 0), -1);
	TEST_CHECK_INT_EQ(toml_parse_string("k = 1", NULL, NULL, 0), -1);
	TEST_CHECK_NULL(toml_get(NULL, "k", &type));
	TEST_CHECK_INT_EQ(toml_get_string(NULL, "k", &s), -1);
	TEST_CHECK_INT_EQ(toml_get_int(NULL, "k", &i), -1);
	TEST_CHECK_INT_EQ(toml_get_double(NULL, "k", NULL), -1);
	TEST_CHECK_INT_EQ(toml_get_bool(NULL, "k", NULL), -1);
	TEST_CHECK_INT_EQ(toml_parse_file(NULL, &t, NULL, 0), -1);
	TEST_CHECK_INT_EQ((long long)toml_array_count(NULL), 0);
	TEST_CHECK_NULL(toml_array_item(NULL, 0, &type));
	toml_free(NULL); /* must be a no-op */
}

int main(void)
{
	TEST_RUN(test_parse_scalars);
	TEST_RUN(test_parse_tables_and_dotted_keys);
	TEST_RUN(test_parse_arrays);
	TEST_RUN(test_parse_comments_and_whitespace);
	TEST_RUN(test_parse_empty);
	TEST_RUN(test_parse_rejects_invalid);
	TEST_RUN(test_errbuf_truncation);
	TEST_RUN(test_parse_file_paths);
	TEST_RUN(test_null_safety);
	return test__failures == 0 ? 0 : 1;
}
