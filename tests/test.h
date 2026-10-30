/**
 * @file test.h
 * @brief Minimal test harness: assertion macros and failure accounting.
 *
 * Each tests/test_*.c is a standalone binary with its own main() that
 * returns 0 on success and 1 if any check failed.
 */

#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>

/** Total number of failed checks in this test binary. */
static int test__failures = 0;

/** Check a boolean condition; report file/line on failure. */
#define TEST_CHECK(cond)                                                     \
	do {                                                                 \
		if (!(cond)) {                                               \
			test__failures++;                                    \
			fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__,      \
				__LINE__, #cond);                            \
		}                                                            \
	} while (0)

/** Check that two values compare equal as long long. */
#define TEST_CHECK_INT_EQ(actual, expected)                                  \
	do {                                                                 \
		long long _ta = (long long)(actual);                         \
		long long _te = (long long)(expected);                       \
		if (_ta != _te) {                                            \
			test__failures++;                                    \
			fprintf(stderr, "  FAIL %s:%d: %s == %s "            \
				"(got %lld, want %lld)\n", __FILE__,         \
				__LINE__, #actual, #expected, _ta, _te);     \
		}                                                            \
	} while (0)

/** Check that two C strings are equal (NULL is never equal). */
#define TEST_CHECK_STR_EQ(actual, expected)                                  \
	do {                                                                 \
		const char *_sa = (actual);                                  \
		const char *_se = (expected);                                \
		if (_sa == NULL || _se == NULL || strcmp(_sa, _se) != 0) {   \
			test__failures++;                                    \
			fprintf(stderr, "  FAIL %s:%d: %s == \"%s\" "        \
				"(got %s)\n", __FILE__, __LINE__, #actual,   \
				_se ? _se : "(null)", _sa ? _sa : "(null)"); \
		}                                                            \
	} while (0)

/** Check that a pointer is NULL. */
#define TEST_CHECK_NULL(p) TEST_CHECK((p) == NULL)

/** Check that a pointer is not NULL. */
#define TEST_CHECK_NOT_NULL(p) TEST_CHECK((p) != NULL)

/** Run one test function and print PASS/FAIL with its name. */
#define TEST_RUN(fn)                                                         \
	do {                                                                 \
		const char *_tn = #fn;                                       \
		int _before = test__failures;                                \
		fn();                                                        \
		printf("%s %s\n",                                            \
		       (test__failures == _before) ? "PASS" : "FAIL",        \
		       _tn);                                                 \
	} while (0)

#endif /* TEST_H */
