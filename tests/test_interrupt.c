/**
 * @file test_interrupt.c
 * @brief Unit tests for src/interrupt.c.
 *
 * The watcher is exercised on a pipe (interrupt_watch_start_fd) so no
 * test touches the real stdin: bytes are written to the pipe and the
 * flag is polled with a deadline. Timing-sensitive waits are generous
 * (hundreds of ms) against a 30 ms ESC timeout to stay robust on slow
 * machines.
 */

#include "test.h"
#include "interrupt.h"

#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* POSIX symbols not declared by strict ISO C99 headers on this toolchain
 * are declared explicitly here instead of enabling feature-test macros
 * (same pattern as tui.c). */
extern int pipe(int pipefd[2]);
extern int close(int fd);

/** Sleeps @p ms milliseconds via select() (portable, no feature macros). */
static void msleep(int ms)
{
	struct timeval tv;

	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	(void)select(0, NULL, NULL, NULL, &tv);
}

/** Polls interrupt_requested() until it equals @p expected or @p ms pass.
 * @return 1 when the flag reached the expected value, 0 on timeout. */
static int wait_flag(int expected, int ms)
{
	while (ms > 0) {
		if (interrupt_requested() == expected)
			return 1;
		msleep(20);
		ms -= 20;
	}
	return interrupt_requested() == expected;
}

static int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t n = write(fd, p, len);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static void test_start_stop_invalid_fd(void)
{
	TEST_CHECK_INT_EQ(interrupt_watch_start_fd(-1), -1);
	/* Stopping a watcher that never ran must be a no-op. */
	interrupt_watch_stop();
	TEST_CHECK_INT_EQ(interrupt_requested(), 0);
}

static void test_double_start_is_single_watcher(void)
{
	int fds[2];

	TEST_CHECK_INT_EQ(pipe(fds), 0);
	TEST_CHECK_INT_EQ(interrupt_watch_start_fd(fds[0]), 0);
	/* Second start while running: no second thread, success. */
	TEST_CHECK_INT_EQ(interrupt_watch_start_fd(fds[0]), 0);
	interrupt_watch_stop();
	TEST_CHECK_INT_EQ(interrupt_requested(), 0);
	close(fds[0]);
	close(fds[1]);
}

static void test_plain_keys_do_not_interrupt(void)
{
	int fds[2];

	TEST_CHECK_INT_EQ(pipe(fds), 0);
	TEST_CHECK_INT_EQ(interrupt_watch_start_fd(fds[0]), 0);
	TEST_CHECK_INT_EQ(write_all(fds[1], "abc\r\n\x7f", 5), 0);
	msleep(200);
	TEST_CHECK_INT_EQ(interrupt_requested(), 0);
	interrupt_watch_stop();
	close(fds[0]);
	close(fds[1]);
}

static void test_lone_esc_interrupts(void)
{
	int fds[2];
	const unsigned char esc = 0x1b;

	TEST_CHECK_INT_EQ(pipe(fds), 0);
	TEST_CHECK_INT_EQ(interrupt_watch_start_fd(fds[0]), 0);
	TEST_CHECK_INT_EQ(write_all(fds[1], &esc, 1), 0);
	TEST_CHECK(wait_flag(1, 1000));
	interrupt_reset();
	TEST_CHECK_INT_EQ(interrupt_requested(), 0);
	interrupt_watch_stop();
	close(fds[0]);
	close(fds[1]);
}

static void test_escape_sequences_do_not_interrupt(void)
{
	int fds[2];

	TEST_CHECK_INT_EQ(pipe(fds), 0);
	TEST_CHECK_INT_EQ(interrupt_watch_start_fd(fds[0]), 0);

	/* Arrow keys: CSI sequences. */
	TEST_CHECK_INT_EQ(write_all(fds[1], "\x1b[A", 3), 0);
	TEST_CHECK_INT_EQ(write_all(fds[1], "\x1b[B", 3), 0);
	TEST_CHECK_INT_EQ(write_all(fds[1], "\x1b[C", 3), 0);
	TEST_CHECK_INT_EQ(write_all(fds[1], "\x1b[D", 3), 0);
	/* CSI with parameters, e.g. Delete (ESC[3~). */
	TEST_CHECK_INT_EQ(write_all(fds[1], "\x1b[3~", 4), 0);
	/* SS3 sequence, e.g. F1 (ESC O P). */
	TEST_CHECK_INT_EQ(write_all(fds[1], "\x1bOP", 3), 0);
	/* Alt-combo: ESC followed by a printable char. */
	TEST_CHECK_INT_EQ(write_all(fds[1], "\x1bx", 2), 0);

	msleep(300);
	TEST_CHECK_INT_EQ(interrupt_requested(), 0);

	/* A lone ESC after all the sequences still interrupts. */
	{
		const unsigned char esc = 0x1b;

		TEST_CHECK_INT_EQ(write_all(fds[1], &esc, 1), 0);
		TEST_CHECK(wait_flag(1, 1000));
	}
	interrupt_reset();
	interrupt_watch_stop();
	close(fds[0]);
	close(fds[1]);
}

static void test_double_esc_interrupts(void)
{
	int fds[2];

	TEST_CHECK_INT_EQ(pipe(fds), 0);
	TEST_CHECK_INT_EQ(interrupt_watch_start_fd(fds[0]), 0);
	/* ESC ESC (burst): at least one interrupt. */
	TEST_CHECK_INT_EQ(write_all(fds[1], "\x1b\x1b", 2), 0);
	TEST_CHECK(wait_flag(1, 1000));
	interrupt_reset();
	interrupt_watch_stop();
	close(fds[0]);
	close(fds[1]);
}

static void test_trigger_and_reset(void)
{
	interrupt_reset();
	TEST_CHECK_INT_EQ(interrupt_requested(), 0);
	interrupt_trigger();
	TEST_CHECK_INT_EQ(interrupt_requested(), 1);
	interrupt_reset();
	TEST_CHECK_INT_EQ(interrupt_requested(), 0);
}

static void test_stop_before_start_is_noop(void)
{
	interrupt_watch_stop();
	TEST_CHECK_INT_EQ(interrupt_requested(), 0);
}

int main(void)
{
	TEST_RUN(test_start_stop_invalid_fd);
	TEST_RUN(test_stop_before_start_is_noop);
	TEST_RUN(test_trigger_and_reset);
	TEST_RUN(test_plain_keys_do_not_interrupt);
	TEST_RUN(test_lone_esc_interrupts);
	TEST_RUN(test_escape_sequences_do_not_interrupt);
	TEST_RUN(test_double_esc_interrupts);
	TEST_RUN(test_double_start_is_single_watcher);

	if (test__failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", test__failures);
		return 1;
	}
	printf("All interrupt tests passed\n");
	return 0;
}
