#!/usr/bin/env python3
"""PTY-based end-to-end check for the transcript-mode TUI.

Runs the real emma-code binary under a pseudo-terminal against the local
mock OpenAI server (tests/mock_server.py) and verifies the transcript
rendering model:

  1. The conversation is printed as normal flowing output into the
     terminal scrollback: messages carry no role labels; user messages
     are full-width background blocks (default theme: blue background
     SGR) and the assistant reply text appears in the byte stream.
  2. No full-screen clear (ESC[2J) is ever emitted: history must survive
     in the terminal's own scrollback, also after exiting.
  3. The streamed reply is rendered incrementally (the reply text arrives
     in chunks, not as one final repaint).

Usage: python3 tests/pty_tui_test.py PORT ./emma-code
Exits 0 on success; prints FAIL diagnostics and exits 1 otherwise.
"""

import os
import pty
import select
import signal
import struct
import sys
import termios
import tempfile
import fcntl
import shutil
import subprocess
import time

TIMEOUT = 15.0


def fail(msg, output=b""):
    print("FAIL pty_tui: %s" % msg)
    if output:
        tail = output[-2000:]
        print("  last output: %r" % tail)
    sys.exit(1)


def read_until(fd, patterns, deadline, output):
    """Reads from fd until all byte patterns appeared or the deadline
    passes. Appends everything read to the output list. Returns True on
    success."""
    while True:
        data = b"".join(output)
        if all(p in data for p in patterns):
            return True
        remaining = deadline - time.time()
        if remaining <= 0:
            return False
        r, _, _ = select.select([fd], [], [], min(remaining, 0.5))
        if not r:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            return False
        if not chunk:
            return False  # EOF: process exited early
        output.append(chunk)


def run_session(binary, scenario, expect_text, extra_patterns=(),
                forbidden=()):
    """Runs one session against the given mock scenario and checks the
    transcript invariants. Returns the collected output on success."""
    extra_patterns = tuple(extra_patterns)
    forbidden = tuple(forbidden)
    tmp = tempfile.mkdtemp(prefix="emma-pty-test-")
    try:
        cfg_dir = os.path.join(tmp, "emma")
        os.makedirs(cfg_dir)
        with open(os.path.join(cfg_dir, "config.toml"), "w") as f:
            f.write('base_url = "http://127.0.0.1:%s/%s/v1"\n'
                    % (os.environ["TEST_MOCK_PORT"], scenario))
            f.write('api_key = "test-key-123"\n')
            f.write('model = "test-model"\n')
            f.write("stream = true\n")

        env = dict(os.environ, XDG_CONFIG_HOME=tmp, TERM="xterm")

        master, slave = pty.openpty()
        # Deterministic 24x80 terminal (the app falls back to 24x80 when
        # the size is unset, but be explicit).
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))

        proc = subprocess.Popen(
            [binary], stdin=slave, stdout=slave, stderr=slave,
            env=env, close_fds=True)
        os.close(slave)

        output = []
        deadline = time.time() + TIMEOUT
        try:
            # 1. The prompt appears once the TUI is up (raw mode active).
            if not read_until(master, [b"You:"], deadline, output):
                fail("input prompt never appeared (app crashed or hung?)",
                     b"".join(output))

            # 2. Send a prompt; the user echo (inside its background
            #    block, default theme: blue bg SGR) and the streamed
            #    reply must appear as flowing transcript output.
            os.write(master, b"Hi\r")
            patterns = [b"You:", b"\x1b[44m", b"Hi", expect_text]
            patterns.extend(extra_patterns)
            if not read_until(master, patterns, deadline, output):
                fail("transcript incomplete after prompt", b"".join(output))

            # 3. Exit; the process must terminate cleanly.
            os.write(master, b"/exit\r")
            try:
                rc = proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.send_signal(signal.SIGKILL)
                fail("process did not exit after /exit", b"".join(output))
            if rc != 0:
                fail("exit code %d after /exit" % rc, b"".join(output))

            data = b"".join(output)
            if b"\x1b[2J" in data:
                fail("full-screen clear (ESC[2J) emitted: scrollback "
                     "history would be destroyed", data)
            for pattern in forbidden:
                if pattern in data:
                    fail("forbidden pattern %r in transcript (role "
                         "labels must be gone)" % pattern, data)
            return data
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    if len(sys.argv) != 3:
        print("usage: pty_tui_test.py PORT BINARY")
        return 1
    port = sys.argv[1]
    binary = sys.argv[2]
    os.environ["TEST_MOCK_PORT"] = port

    if not os.access(binary, os.X_OK):
        print("SKIP pty_tui: %s not built" % binary)
        return 0

    # Short reply: the basic transcript flow. Assistant lines carry the
    # same 1-column indent as the text inside user background blocks.
    run_session(binary, "aistream", b" Hello from mock!")
    # Long multi-paragraph reply: completed lines stream into the
    # scrollback while the reply is still arriving (indented too).
    run_session(binary, "longstream", b" Final short line.")
    # Long reasoning stream in small chunks: the reasoning text must
    # appear contiguously (wrapped lines emitted at stable wrap
    # boundaries), not as one fragment per streamed chunk, and it must
    # be styled with the default italic (dim+italic SGR) reasoning.
    # Reasoning lines carry the same 1-column indent as content lines:
    # every styled reasoning line starts with one space before the
    # SGR sequence (wrap-boundary independent), and an empty indented
    # line separates the reasoning block from the content.
    run_session(binary, "longreason", b" Done.",
                extra_patterns=[b" \x1b[2;3m",
                                b"\x1b[2;3m",
                                b"minimal example. First",
                                b"statement handling the",
                                b"avoid the underflow",
                                # Empty indented line between the reasoning
                                # block and the content: the only display
                                # line that is exactly the pad column.
                                b" \r\n"])

    # Reply starting with blank lines (as many chat APIs emit): the
    # assistant section must not begin with an empty row. Before the
    # leading-breaks trim, two blank display lines were emitted right
    # after a live-area erase (ESC[J); the message separator line sits
    # between the sections in the byte stream, so the erase boundary is
    # what makes this pattern unambiguous.
    run_session(binary, "leadnl", b" Hello from mock!",
                forbidden=[b"\x1b[J\r\n\r\n"])

    # Role labels are gone everywhere: "Assistant: "/"User: " must not
    # appear in any transcript.
    run_session(binary, "aistream", b"Hello from mock!",
                forbidden=[b"Assistant: ", b"User: "])

    print("PASS pty_tui: transcript mode keeps history in the scrollback")
    return 0


if __name__ == "__main__":
    sys.exit(main())
