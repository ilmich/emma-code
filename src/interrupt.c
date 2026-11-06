/**
 * @file interrupt.c
 * @brief ESC-key interruption of the in-flight assistant request.
 *
 * A watcher thread selects on the watched fd (stdin in production, a
 * pipe in tests) plus a self-pipe used to wake it on stop(). Keystrokes
 * read while the watcher is active are discarded; a lone ESC (0x1b not
 * followed by a sequence byte within ESC_WAIT_MS) raises the interrupt
 * flag. POSIX-only symbols (pipe/close/read/write, pthreads, select) are
 * declared/included explicitly instead of enabling feature-test macros
 * (see AGENTS.md portability rules); <unistd.h>/<sys/select.h> are the
 * same headers tui.c already relies on.
 */

#include "interrupt.h"

#include <pthread.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>

/* POSIX symbols not declared by strict ISO C99 headers on this toolchain
 * are declared explicitly here instead of enabling feature-test macros
 * (same pattern as tui.c). */
extern int pipe(int pipefd[2]);
extern int close(int fd);

/** How long (ms) to wait for a byte after ESC before calling it lone. */
#define ESC_WAIT_MS 30

/** Guard for the CSI-consumption loop (a sequence is never longer). */
#define CSI_MAX_BYTES 32

struct watch_ctx {
	int fd;       /**< watched fd (stdin in production) */
	int wake_fd;  /**< read end of the wake self-pipe (-1: none) */
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_running;          /**< guarded by g_lock */
static int g_interrupt;                 /**< guarded by g_lock */
static pthread_t g_tid;                 /**< guarded by g_lock */
static struct watch_ctx *g_ctx;         /**< guarded by g_lock */
static int g_wake_fd = -1;              /**< write end of the wake pipe */

static int watch_running(void)
{
	int running;

	pthread_mutex_lock(&g_lock);
	running = g_running;
	pthread_mutex_unlock(&g_lock);
	return running;
}

static void set_interrupt(int value)
{
	pthread_mutex_lock(&g_lock);
	g_interrupt = value;
	pthread_mutex_unlock(&g_lock);
}

/**
 * @brief Consumes one complete escape sequence (already past ESC and the
 *        introducer byte @p b2) from @p fd, discarding every byte.
 *
 * CSI sequences (ESC [ ... final) end at a byte in 0x40..0x7E; SS3
 * sequences (ESC O x) take exactly one more byte. A bounded guard plus a
 * short per-byte timeout keeps a truncated sequence from hanging the
 * watcher.
 */
static void consume_escape_sequence(int fd, unsigned char b2)
{
	int guard = 0;

	while (guard++ < CSI_MAX_BYTES) {
		fd_set rfds;
		struct timeval tv;
		unsigned char b;

		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = 100 * 1000;
		if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
			return;
		if (read(fd, &b, 1) != 1)
			return;
		if (b2 == 'O')
			return; /* SS3: exactly one byte after 'O' */
		if (b >= 0x40 && b <= 0x7e)
			return; /* CSI final byte */
	}
}

static void *watch_thread(void *arg)
{
	struct watch_ctx *ctx = arg;

	while (watch_running()) {
		fd_set rfds;
		int maxfd;
		int rc;
		unsigned char b;

		FD_ZERO(&rfds);
		FD_SET(ctx->fd, &rfds);
		maxfd = ctx->fd;
		if (ctx->wake_fd >= 0) {
			FD_SET(ctx->wake_fd, &rfds);
			if (ctx->wake_fd > maxfd)
				maxfd = ctx->wake_fd;
		}
		rc = select(maxfd + 1, &rfds, NULL, NULL, NULL);
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (ctx->wake_fd >= 0 && FD_ISSET(ctx->wake_fd, &rfds)) {
			char drain[64];

			(void)read(ctx->wake_fd, drain, sizeof(drain));
			continue; /* re-check watch_running() */
		}
		if (rc == 0 || !FD_ISSET(ctx->fd, &rfds))
			continue;

		if (read(ctx->fd, &b, 1) != 1)
			break; /* EOF or unrecoverable error */

		if (b != 0x1b)
			continue; /* ordinary keystroke: discard */

		/* ESC: a lone keypress sends exactly one byte, while
		 * escape sequences (arrow keys, ...) arrive as one burst.
		 * Wait briefly for a follower before deciding. */
		FD_ZERO(&rfds);
		FD_SET(ctx->fd, &rfds);
		{
			struct timeval tv;

			tv.tv_sec = 0;
			tv.tv_usec = ESC_WAIT_MS * 1000;
			rc = select(ctx->fd + 1, &rfds, NULL, NULL, &tv);
		}
		if (rc <= 0 || !FD_ISSET(ctx->fd, &rfds)) {
			set_interrupt(1); /* lone ESC */
			continue;
		}
		{
			unsigned char b2;

			if (read(ctx->fd, &b2, 1) != 1) {
				set_interrupt(1);
				break;
			}
			if (b2 == 0x1b) {
				set_interrupt(1); /* ESC ESC: interrupt */
				continue;
			}
			if (b2 == '[' || b2 == 'O')
				consume_escape_sequence(ctx->fd, b2);
			/* anything else (e.g. Alt-combo): discard */
		}
	}
	return NULL;
}

int interrupt_watch_start_fd(int fd)
{
	int wake[2] = {-1, -1};
	struct watch_ctx *ctx = NULL;
	pthread_t tid;
	int started;

	if (fd < 0)
		return -1;

	pthread_mutex_lock(&g_lock);
	started = g_running;
	pthread_mutex_unlock(&g_lock);
	if (started)
		return 0; /* already watching: nothing to do */

	ctx = malloc(sizeof(*ctx));
	if (ctx == NULL)
		return -1;
	ctx->fd = fd;
	ctx->wake_fd = -1;
	if (pipe(wake) != 0) {
		free(ctx);
		return -1;
	}
	ctx->wake_fd = wake[0];

	interrupt_reset();
	pthread_mutex_lock(&g_lock);
	g_running = 1;
	pthread_mutex_unlock(&g_lock);
	if (pthread_create(&tid, NULL, watch_thread, ctx) != 0) {
		pthread_mutex_lock(&g_lock);
		g_running = 0;
		pthread_mutex_unlock(&g_lock);
		close(wake[0]);
		close(wake[1]);
		free(ctx);
		return -1;
	}

	pthread_mutex_lock(&g_lock);
	g_tid = tid;
	g_ctx = ctx;
	g_wake_fd = wake[1];
	pthread_mutex_unlock(&g_lock);
	return 0;
}

int interrupt_watch_start(void)
{
	return interrupt_watch_start_fd(STDIN_FILENO);
}

void interrupt_watch_stop(void)
{
	pthread_t tid;
	struct watch_ctx *ctx;
	int wake_fd;
	int running;

	pthread_mutex_lock(&g_lock);
	running = g_running;
	tid = g_tid;
	ctx = g_ctx;
	wake_fd = g_wake_fd;
	g_running = 0;
	g_tid = (pthread_t)0;
	g_ctx = NULL;
	g_wake_fd = -1;
	pthread_mutex_unlock(&g_lock);

	if (!running)
		return;

	if (wake_fd >= 0) {
		char wake_byte = 1;
		ssize_t nwritten = write(wake_fd, &wake_byte, 1);

		(void)nwritten; /* best effort: the thread also re-checks
				 * watch_running() on every loop */
	}
	pthread_join(tid, NULL);
	if (ctx != NULL) {
		if (ctx->wake_fd >= 0)
			close(ctx->wake_fd);
		free(ctx);
	}
	if (wake_fd >= 0)
		close(wake_fd);
}

int interrupt_requested(void)
{
	int requested;

	pthread_mutex_lock(&g_lock);
	requested = g_interrupt;
	pthread_mutex_unlock(&g_lock);
	return requested;
}

void interrupt_trigger(void)
{
	set_interrupt(1);
}

void interrupt_reset(void)
{
	set_interrupt(0);
}
