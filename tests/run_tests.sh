#!/bin/sh
# Test runner: executes the compiled unit test binaries.
# The network suite needs the local mock server (tests/mock_server.py),
# which publishes its ephemeral port on stdout.
set -u

MOCK_PID=""
cleanup() {
    if [ -n "$MOCK_PID" ]; then
        kill "$MOCK_PID" >/dev/null 2>&1
    fi
}
trap cleanup EXIT INT TERM

status=0

./build/tests/test_session_context || status=1
./build/tests/test_toml || status=1
./build/tests/test_lua_scripting || status=1
./build/tests/test_tui || status=1
./build/tests/test_commands || status=1

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP network tests: python3 not found" >&2
    exit 1
fi

: > build/mock_port.txt
python3 tests/mock_server.py > build/mock_port.txt 2> build/mock_server.log &
MOCK_PID=$!

# Wait for the mock server to publish its port.
tries=0
while [ ! -s build/mock_port.txt ] && [ "$tries" -lt 50 ]; do
    sleep 0.1
    tries=$((tries + 1))
done

if [ ! -s build/mock_port.txt ]; then
    echo "FAIL mock server did not start (see build/mock_server.log)" >&2
    exit 1
fi

TEST_MOCK_PORT=$(cat build/mock_port.txt)
export TEST_MOCK_PORT

./build/tests/test_http_client || status=1
./build/tests/test_openai_client || status=1

if [ "$status" -eq 0 ]; then
    echo "All test suites passed"
fi
exit $status
