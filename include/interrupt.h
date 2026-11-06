/**
 * @file interrupt.h
 * @brief ESC-key interruption of the in-flight assistant request.
 *
 * While the agent waits for the model (streaming or not), nobody reads
 * stdin: the request is a blocking curl_easy_perform(). This module
 * provides a small watcher thread that, while active, reads stdin bytes
 * and discards them (type-ahead is intentionally dropped) and raises an
 * interrupt flag when the user presses a lone ESC key. Escape sequences
 * (arrow keys, CSI/SS3, Alt-combos) never trigger the interrupt.
 *
 * The watcher is meant to be started only around blocking completion
 * calls (see agent_loop.c): it must never run while tui_read_input() or
 * tui_confirm() own stdin, or it would steal their keystrokes.
 */

#ifndef INTERRUPT_H
#define INTERRUPT_H

/**
 * @brief Starts the watcher on stdin.
 *
 * Convenience wrapper for interrupt_watch_start_fd(STDIN_FILENO).
 * Does nothing (returns 0) when a watcher is already running.
 *
 * @return 0 on success (or already running), -1 on failure (thread or
 *         wake-pipe creation failed; the caller degrades gracefully).
 */
int interrupt_watch_start(void);

/**
 * @brief Starts the watcher on an arbitrary readable fd.
 *
 * Testability seam: production passes stdin, tests pass a pipe. The
 * flag is reset on start. Only one watcher may run at a time.
 *
 * @param fd Readable file descriptor to watch (>= 0).
 * @return 0 on success (or already running), -1 on failure.
 */
int interrupt_watch_start_fd(int fd);

/**
 * @brief Stops the watcher and joins its thread.
 *
 * Safe to call when no watcher is running. Blocks until the thread has
 * exited, so stdin is free again for tui_read_input()/tui_confirm().
 * The interrupt flag is left untouched (the caller decides whether to
 * reset it).
 */
void interrupt_watch_stop(void);

/**
 * @brief Whether an interrupt (lone ESC) has been requested.
 *
 * @return Nonzero when an interrupt is pending, 0 otherwise.
 */
int interrupt_requested(void);

/**
 * @brief Raises the interrupt flag.
 *
 * Normally raised by the watcher thread on a lone ESC; exposed so tests
 * (and future input sources) can trigger the same code path without a
 * real keypress.
 */
void interrupt_trigger(void);

/**
 * @brief Clears the interrupt flag.
 *
 * Called before every completion request so a stale flag can never
 * abort the next turn.
 */
void interrupt_reset(void);

#endif /* INTERRUPT_H */
